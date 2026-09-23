#include "guest_execution.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <limits>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfr {
std::atomic<uint64_t> GuestExecution::main_thread_ready_wait_ns{0};
std::atomic<uint64_t> GuestExecution::main_thread_blocked_ns{0};
std::atomic<uint64_t> GuestExecution::held_ns[64]{};

GuestExecutionCancelled::GuestExecutionCancelled()
    : std::runtime_error("guest execution cancelled") {}

struct GuestExecution::State {
    struct Waiter {
        uint64_t guest_id;
        std::thread::id host_thread;
        bool urgent = false;  // queued ahead of ordinary waiters
    };

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Waiter>> ready;
    std::vector<Waiter> blocked;
    uint64_t owner_id = 0;
    uint64_t next_completion_id = uint64_t{1} << 32;
    std::thread::id owner_thread;
    bool stopping = false;
    // Mirrors stopping for the lock-free checkpoint fast path.
    std::atomic<bool> stopping_flag{false};
    std::atomic<int64_t> quantum_us{0};
    // Time-critical guests: woken from a wait they are queued ahead of
    // ordinary waiters and the owner yields at its next checkpoint, as a
    // console core would run them at once. Rotation after a quantum stays FIFO.
    std::unordered_set<uint64_t> urgent_ids;
    std::atomic<uint32_t> urgent_waiting{0};

    void enqueue(const std::shared_ptr<Waiter>& waiter, bool woken) {
        if (woken && urgent_ids.contains(waiter->guest_id)) {
            waiter->urgent = true;
            const auto position = std::find_if(ready.begin(), ready.end(), [](const auto& w) { return !w->urgent; });
            ready.insert(position, waiter);
            urgent_waiting.fetch_add(1, std::memory_order_relaxed);
        } else {
            waiter->urgent = false;
            ready.push_back(waiter);
        }
    }
    void remove(std::deque<std::shared_ptr<Waiter>>::iterator position) {
        if ((*position)->urgent) urgent_waiting.fetch_sub(1, std::memory_order_relaxed);
        ready.erase(position);
    }
    void pop_front() { remove(ready.begin()); }
    std::exception_ptr failure;
    std::stop_source stop_source;
    std::vector<std::weak_ptr<State>> followers;
    // Stops the followers too; the caller has set stopping and released the mutex.
    void stop_followers() noexcept;
};

GuestExecution::Standing GuestExecution::standing() const {
    Standing result;
    std::lock_guard lock(state_->mutex);
    result.owner = state_->owner_id;
    for (const auto& waiter : state_->ready) result.ready.push_back(waiter->guest_id);
    for (const auto& waiter : state_->blocked) result.blocked.push_back(waiter.guest_id);
    return result;
}

void GuestExecution::State::stop_followers() noexcept {
    std::vector<std::shared_ptr<State>> targets;
    {
        std::lock_guard lock(mutex);
        for (const auto& weak : followers)
            if (auto follower = weak.lock()) targets.push_back(std::move(follower));
    }
    for (const auto& follower : targets) {
        std::stop_source source(std::nostopstate);
        {
            std::lock_guard lock(follower->mutex);
            follower->stopping = true;
            follower->stopping_flag.store(true, std::memory_order_relaxed);
            source = follower->stop_source;
            follower->changed.notify_all();
        }
        source.request_stop();
        follower->stop_followers();
    }
}

void GuestExecution::add_follower(GuestExecution& follower) {
    std::lock_guard lock(state_->mutex);
    state_->followers.push_back(follower.state_);
}

std::stop_token GuestExecution::stop_token() const {
    std::lock_guard lock(state_->mutex);
    return state_->stop_source.get_token();
}

GuestExecution::GuestExecution() : state_(std::make_shared<State>()) {}
GuestExecution::~GuestExecution() = default;

std::unique_ptr<GuestExecution::Lease> GuestExecution::enter_completion() {
    uint64_t identity;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) throw GuestExecutionCancelled();
        if (state_->next_completion_id == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("host completion execution identities exhausted");
        identity = state_->next_completion_id++;
    }
    return enter_identity(identity);
}

GuestExecution::Lease::Lease(std::shared_ptr<State> state, uint64_t identity)
    : state_(std::move(state)), guest_id_(identity) {}
GuestExecution::Lease::~Lease() noexcept {
    if (state_ && detached_) {
        // Unwinding out of detached guest code (cancellation): drop the
        // entry that kept the identity active.
        std::lock_guard lock(state_->mutex);
        const auto blocked = std::find_if(state_->blocked.begin(), state_->blocked.end(), [&](const auto& entry) {
            return entry.guest_id == guest_id_ && entry.host_thread == std::this_thread::get_id();
        });
        if (blocked != state_->blocked.end()) state_->blocked.erase(blocked);
        state_->changed.notify_all();
        return;
    }
    if (!state_ || !owns_) return;
    std::lock_guard lock(state_->mutex);
    if (state_->owner_id == guest_id_) {
        released();
        state_->owner_id = 0;
        state_->owner_thread = {};
        state_->changed.notify_all();
    }
}

void GuestExecution::Lease::released() {
    if (guest_id_ < 64)
        held_ns[guest_id_].fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - held_since_).count()), std::memory_order_relaxed);
}

void GuestExecution::Lease::acquired() {
    owner_thread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    acquired_at_ = std::chrono::steady_clock::now();
    held_since_ = acquired_at_;
    checkpoints_ = 0;
    urgent_owner_ = state_->urgent_ids.contains(guest_id_);  // callers hold the mutex
}

void GuestExecution::Lease::renew_quantum() {
    acquired_at_ = std::chrono::steady_clock::now();
}

void GuestExecution::Lease::checkpoint(bool allow_handoff) {
    if (detached_) {
        if (state_->stopping_flag.load(std::memory_order_relaxed)) throw GuestExecutionCancelled();
        return;
    }
    // Fast path: guest code checkpoints at every function entry. Only the
    // owner may call it, stopping is observed at once, and a handoff to a
    // ready guest happens after the owner has run for a scheduling quantum.
    if (owns_ && owner_thread_.load(std::memory_order_relaxed) == std::this_thread::get_id() &&
        !state_->stopping_flag.load(std::memory_order_relaxed)) {
        if (!allow_handoff) return;
        const auto quantum = std::chrono::microseconds(state_->quantum_us.load(std::memory_order_relaxed));
        // A waiting time-critical guest preempts an ordinary owner at once;
        // another time-critical owner keeps running until it blocks or yields.
        if (quantum.count() > 0 && (urgent_owner_ || !state_->urgent_waiting.load(std::memory_order_relaxed))) {
            if (++checkpoints_ % 64) return;
            if (std::chrono::steady_clock::now() - acquired_at_ < quantum) return;
        }
    }
    std::unique_lock lock(state_->mutex);
    if (state_->owner_id != guest_id_ || state_->owner_thread != std::this_thread::get_id())
        throw std::logic_error("guest execution lease used outside its owning host thread");
    if (state_->stopping) throw GuestExecutionCancelled();
    if (!allow_handoff) return;
    // A time-critical owner keeps running past the quantum (up to 20 ms)
    // while only ordinary guests wait, as it would hold its console core.
    if (state_->ready.empty() || (urgent_owner_ &&
            std::chrono::steady_clock::now() - acquired_at_ < std::chrono::milliseconds(20))) {
        if (state_->ready.empty()) acquired_at_ = std::chrono::steady_clock::now();
        return;
    }

    auto self = std::make_shared<State::Waiter>(State::Waiter{guest_id_, std::this_thread::get_id()});
    state_->enqueue(self, false);
    released();
    owns_ = false;
    state_->owner_id = 0;
    state_->owner_thread = {};
    state_->changed.notify_all();
    // The companion is never held while waiting for this permit.
    const bool companion = companion_ && !companion_->detached();
    if (companion) {
        lock.unlock();
        companion_->detach();
        lock.lock();
    }
    const auto queued_at = std::chrono::steady_clock::now();
    state_->changed.wait(lock, [&] {
        return state_->stopping ||
            (state_->owner_id == 0 && !state_->ready.empty() && state_->ready.front() == self);
    });
    if (guest_id_ == 1)
        main_thread_ready_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - queued_at).count()), std::memory_order_relaxed);
    if (state_->stopping) {
        const auto position = std::find(state_->ready.begin(), state_->ready.end(), self);
        if (position != state_->ready.end()) state_->remove(position);
        state_->changed.notify_all();
        throw GuestExecutionCancelled();
    }
    state_->pop_front();
    state_->owner_id = guest_id_;
    state_->owner_thread = std::this_thread::get_id();
    owns_ = true;
    acquired();
    state_->changed.notify_all();
    if (companion) {
        lock.unlock();
        companion_->attach();
    }
}

namespace {
std::mutex block_count_mutex;
std::condition_variable_any block_counted;
std::unordered_map<uint32_t, uint64_t> block_counts;
void count_block(uint32_t guest_id) {
    {
        std::lock_guard lock(block_count_mutex);
        ++block_counts[guest_id];
    }
    block_counted.notify_all();
}
}

uint64_t GuestExecution::blocking_waits(uint32_t guest_id) {
    std::lock_guard lock(block_count_mutex);
    const auto found = block_counts.find(guest_id);
    return found == block_counts.end() ? 0 : found->second;
}

void GuestExecution::wait_for_block(uint32_t guest_id, uint64_t after, std::chrono::milliseconds limit,
                                    std::stop_token stop) {
    std::unique_lock lock(block_count_mutex);
    block_counted.wait_for(lock, stop, limit, [&] {
        const auto found = block_counts.find(guest_id);
        return found != block_counts.end() && found->second > after;
    });
}

void GuestExecution::Lease::run_blocking(std::function<void(std::stop_token)> operation,
                                        std::function<void()> before_release) {
    if (!operation) throw std::logic_error("blocking guest operation is required");
    auto self = std::make_shared<State::Waiter>(State::Waiter{guest_id_, std::this_thread::get_id()});
    std::stop_token stop_token;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->owner_id != guest_id_ || state_->owner_thread != std::this_thread::get_id())
            throw std::logic_error("guest execution lease used outside its owning host thread");
        if (state_->stopping) throw GuestExecutionCancelled();
        state_->blocked.push_back(*self);
        try {
            if (before_release) before_release();
        } catch (...) {
            // Admission is private until the mutex is released. Preserve the
            // original owner and remove only the entry prepared for this call.
            state_->blocked.pop_back();
            throw;
        }
        stop_token = state_->stop_source.get_token();
        released();
        owns_ = false;
        state_->owner_id = 0;
        state_->owner_thread = {};
        state_->changed.notify_all();
    }
    if (guest_id_ && guest_id_ <= 0xFFFFFFFFu) count_block(uint32_t(guest_id_));

    std::exception_ptr operation_failure;
    const auto blocked_at = std::chrono::steady_clock::now();
    try {
        if (companion_) companion_->detach();
        operation(stop_token);
    }
    catch (...) { operation_failure = std::current_exception(); }
    if (guest_id_ == 1)
        main_thread_blocked_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - blocked_at).count()), std::memory_order_relaxed);

    std::unique_lock lock(state_->mutex);
    const auto blocked = std::find_if(state_->blocked.begin(), state_->blocked.end(), [&](const auto& entry) {
        return entry.guest_id == guest_id_ && entry.host_thread == std::this_thread::get_id();
    });
    if (blocked != state_->blocked.end()) state_->blocked.erase(blocked);
    if (operation_failure && !state_->failure) state_->failure = operation_failure;
    if (state_->stopping) {
        state_->changed.notify_all();
        lock.unlock();
        if (operation_failure) std::rethrow_exception(operation_failure);
        throw GuestExecutionCancelled();
    }

    try { state_->enqueue(self, true); }
    catch (...) {
        state_->changed.notify_all();
        throw;
    }
    state_->changed.notify_all();
    const auto queued_at = std::chrono::steady_clock::now();
    state_->changed.wait(lock, [&] {
        return state_->stopping ||
            (state_->owner_id == 0 && !state_->ready.empty() && state_->ready.front() == self);
    });
    if (guest_id_ == 1)
        main_thread_ready_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - queued_at).count()), std::memory_order_relaxed);
    if (state_->stopping) {
        const auto position = std::find(state_->ready.begin(), state_->ready.end(), self);
        if (position != state_->ready.end()) state_->remove(position);
        state_->changed.notify_all();
        lock.unlock();
        if (operation_failure) std::rethrow_exception(operation_failure);
        throw GuestExecutionCancelled();
    }
    state_->pop_front();
    state_->owner_id = guest_id_;
    state_->owner_thread = std::this_thread::get_id();
    owns_ = true;
    acquired();
    state_->changed.notify_all();
    lock.unlock();
    if (companion_ && companion_->detached()) companion_->attach();
    if (operation_failure) std::rethrow_exception(operation_failure);
    if (after_blocking_) after_blocking_();
}

void GuestExecution::Lease::detach() {
    std::lock_guard lock(state_->mutex);
    if (detached_ || state_->owner_id != guest_id_ || state_->owner_thread != std::this_thread::get_id())
        throw std::logic_error("guest execution lease detached outside its owning host thread");
    if (state_->stopping) throw GuestExecutionCancelled();
    // Listed as blocked: the identity stays active, so it cannot enter twice.
    state_->blocked.push_back(State::Waiter{guest_id_, std::this_thread::get_id()});
    released();
    owns_ = false;
    detached_ = true;
    state_->owner_id = 0;
    state_->owner_thread = {};
    state_->changed.notify_all();
}

void GuestExecution::Lease::attach() {
    if (!detached_) throw std::logic_error("guest execution lease attached without detaching");
    // The companion is taken after this permit, never held while waiting for it.
    if (companion_ && !companion_->detached()) companion_->detach();
    attach_self();
    if (companion_) companion_->attach();
}

void GuestExecution::Lease::attach_self() {
    std::unique_lock lock(state_->mutex);
    const auto blocked = std::find_if(state_->blocked.begin(), state_->blocked.end(), [&](const auto& entry) {
        return entry.guest_id == guest_id_ && entry.host_thread == std::this_thread::get_id();
    });
    if (blocked != state_->blocked.end()) state_->blocked.erase(blocked);
    detached_ = false;
    if (state_->stopping) {
        state_->changed.notify_all();
        throw GuestExecutionCancelled();
    }
    auto self = std::make_shared<State::Waiter>(State::Waiter{guest_id_, std::this_thread::get_id()});
    state_->enqueue(self, true);
    state_->changed.notify_all();
    state_->changed.wait(lock, [&] {
        return state_->stopping ||
            (state_->owner_id == 0 && !state_->ready.empty() && state_->ready.front() == self);
    });
    if (state_->stopping) {
        const auto position = std::find(state_->ready.begin(), state_->ready.end(), self);
        if (position != state_->ready.end()) state_->remove(position);
        state_->changed.notify_all();
        throw GuestExecutionCancelled();
    }
    state_->pop_front();
    state_->owner_id = guest_id_;
    state_->owner_thread = std::this_thread::get_id();
    owns_ = true;
    acquired();
    state_->changed.notify_all();
}

void GuestExecution::set_urgent(uint32_t guest_id, bool urgent) {
    std::lock_guard lock(state_->mutex);
    if (urgent) state_->urgent_ids.insert(guest_id);
    else state_->urgent_ids.erase(guest_id);
}

void GuestExecution::set_scheduling_quantum(std::chrono::microseconds quantum) {
    state_->quantum_us.store(quantum.count(), std::memory_order_relaxed);
}

std::unique_ptr<GuestExecution::Lease> GuestExecution::enter(uint32_t guest_id) {
    return enter_identity(guest_id);
}

std::unique_ptr<GuestExecution::Lease> GuestExecution::enter_identity(uint64_t guest_id) {
    if (!guest_id) throw std::logic_error("guest execution ID must be nonzero");
    std::unique_lock lock(state_->mutex);
    if (state_->stopping) throw GuestExecutionCancelled();
    if (state_->owner_id == guest_id || std::any_of(state_->ready.begin(), state_->ready.end(),
            [&](const auto& waiter) { return waiter->guest_id == guest_id; }) ||
            std::any_of(state_->blocked.begin(), state_->blocked.end(),
                [&](const auto& waiter) { return waiter.guest_id == guest_id; }))
        throw std::logic_error("guest execution ID is already active or queued");
    if ((state_->owner_id != 0 && state_->owner_thread == std::this_thread::get_id()) ||
            std::any_of(state_->blocked.begin(), state_->blocked.end(),
                [&](const auto& waiter) { return waiter.host_thread == std::this_thread::get_id(); }))
        throw std::logic_error("owning host thread cannot enter another guest execution");

    auto lease = std::unique_ptr<Lease>(new Lease(state_, guest_id));
    auto self = std::make_shared<State::Waiter>(State::Waiter{guest_id, std::this_thread::get_id()});
    state_->enqueue(self, true);
    state_->changed.notify_all();
    state_->changed.wait(lock, [&] {
        return state_->stopping ||
            (state_->owner_id == 0 && !state_->ready.empty() && state_->ready.front() == self);
    });
    if (state_->stopping) {
        const auto position = std::find(state_->ready.begin(), state_->ready.end(), self);
        if (position != state_->ready.end()) state_->remove(position);
        state_->changed.notify_all();
        throw GuestExecutionCancelled();
    }
    state_->pop_front();
    state_->owner_id = guest_id;
    state_->owner_thread = std::this_thread::get_id();
    lease->owns_ = true;
    lease->acquired();
    state_->changed.notify_all();
    return lease;
}

// How long an owner waits for a thread it resumed, and how often that ran
// out (reported by the hang report; a healthy run never does).
constexpr auto ready_wait_limit = std::chrono::milliseconds(50);
std::atomic<uint64_t> ready_wait_timeouts{0};

uint64_t GuestExecution::resume_wait_timeouts() noexcept { return ready_wait_timeouts.load(std::memory_order_relaxed); }

void GuestExecution::wait_until_ready(uint32_t guest_id) {
    if (!guest_id) throw std::logic_error("ready guest ID must be nonzero");
    std::unique_lock lock(state_->mutex);
    if (state_->owner_id == 0 || state_->owner_thread != std::this_thread::get_id())
        throw std::logic_error("ready wait requires the owning host thread");
    if (state_->owner_id == guest_id)
        throw std::logic_error("ready wait target must differ from the current owner");
    if (state_->stopping) throw GuestExecutionCancelled();
    // A target already inside a wait of its own queues when that wait ends,
    // which may be work this owner has yet to do: waiting for it here, with
    // the permit held, is a cycle. It deadlocked a race at the same frame two
    // runs in three with the thread start delay off. The bounded wait is the
    // backstop for any other way the target never queues.
    const auto queued_or_waiting = [&] {
        return state_->stopping ||
               std::any_of(state_->ready.begin(), state_->ready.end(),
                           [&](const auto& waiter) { return waiter->guest_id == guest_id; }) ||
               std::any_of(state_->blocked.begin(), state_->blocked.end(),
                           [&](const auto& waiter) { return waiter.guest_id == guest_id; });
    };
    if (!state_->changed.wait_for(lock, ready_wait_limit, queued_or_waiting)) ++ready_wait_timeouts;
    if (state_->stopping) throw GuestExecutionCancelled();
}

void GuestExecution::fail(std::exception_ptr failure) noexcept {
    std::stop_source stop_source(std::nostopstate);
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->failure && failure) state_->failure = failure;
        state_->stopping = true;
        state_->stopping_flag.store(true, std::memory_order_relaxed);
        stop_source = state_->stop_source;
        state_->changed.notify_all();
    }
    stop_source.request_stop();
    state_->stop_followers();
}

void GuestExecution::stop() noexcept {
    std::stop_source stop_source(std::nostopstate);
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->stopping_flag.store(true, std::memory_order_relaxed);
        stop_source = state_->stop_source;
        state_->changed.notify_all();
    }
    stop_source.request_stop();
    state_->stop_followers();
}

void GuestExecution::stop_and_drain() noexcept {
    std::unique_lock lock(state_->mutex);
    if (state_->owner_id != 0 && state_->owner_thread == std::this_thread::get_id())
        std::terminate();
    state_->stopping = true;
    state_->stopping_flag.store(true, std::memory_order_relaxed);
    std::stop_source stop_source(std::nostopstate);
    stop_source = state_->stop_source;
    state_->changed.notify_all();
    lock.unlock();
    stop_source.request_stop();
    state_->stop_followers();
    lock.lock();
    state_->changed.wait(lock, [&] { return state_->owner_id == 0; });
}

void GuestExecution::rethrow_failure() const {
    std::exception_ptr failure;
    {
        std::lock_guard lock(state_->mutex);
        failure = state_->failure;
    }
    if (failure) std::rethrow_exception(failure);
}

bool GuestExecution::stopped() const {
    std::lock_guard lock(state_->mutex);
    return state_->stopping;
}

}
