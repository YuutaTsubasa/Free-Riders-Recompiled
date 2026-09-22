#include "guest_critical_sections.h"
#include "critical_section.h"
#include "guest_memory.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sfr {
namespace { constexpr uint32_t max_recursion = 0x7fffffffu; }

struct GuestCriticalSections::Waiter {
    uint32_t address, thread;
    bool selected = false, completed = false;
};
struct GuestCriticalSections::Shared {
    explicit Shared(GuestMemory& value) : memory(value) {}
    GuestMemory& memory;
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<uint32_t, std::deque<std::shared_ptr<Waiter>>> queues;
    std::unordered_set<uint32_t> active;
    bool stopping = false;
};

namespace {
struct State { uint32_t lock, recursion, owner; };
[[noreturn]] void bad(uint32_t address, const char* detail) {
    throw RuntimeStop("critical-section-invalid", address, detail);
}
void running(const GuestCriticalSections::Shared& s, uint32_t address) {
    if (s.stopping) throw RuntimeStop("critical-section-contention", address,
                                      "critical-section coordinator is stopped");
}
void preflight(GuestCriticalSections::Shared& s, uint32_t address, uint32_t thread,
               bool needs_thread = true) {
    if (address & 3u) bad(address, "critical section must be 4-byte aligned");
    if (needs_thread && !thread) bad(address, "current guest thread object is null");
    if (s.memory.has_reservation())
        throw RuntimeStop("critical-section-reservation", address,
                          "critical-section operation during a live reservation");
    s.memory.check_write(uint64_t(address), 28);
}
State read(GuestCriticalSections::Shared& s, uint32_t address) {
    if (s.memory.load<uint8_t>(address) != 1 ||
        s.memory.load<uint32_t>(uint64_t(address) + 4) != 0)
        bad(address, "expected an unsignaled critical-section event");
    return {s.memory.load<uint32_t>(uint64_t(address) + 16),
            s.memory.load<uint32_t>(uint64_t(address) + 20),
            s.memory.load<uint32_t>(uint64_t(address) + 24)};
}
auto queue(GuestCriticalSections::Shared& s, uint32_t address) {
    auto found = s.queues.find(address);
    return found == s.queues.end() ? nullptr : &found->second;
}
State checked(GuestCriticalSections::Shared& s, uint32_t address) {
    State v = read(s, address);
    auto* q = queue(s, address);
    const uint64_t waiters = q ? q->size() : 0;
    if (v.owner) {
        if (!v.recursion || v.recursion > max_recursion || v.lock > 0x7fffffffu ||
            uint64_t(v.recursion - 1) + waiters != v.lock ||
            (q && !q->empty() && q->front()->selected))
            bad(address, "owned state does not match admitted waiters");
    } else if (v.recursion) {
        bad(address, "unowned critical section has recursion");
    } else if (!waiters) {
        if (v.lock != 0xffffffffu) bad(address, "free critical section has nonfree count");
    } else if (v.lock > 0x7fffffffu || !q->front()->selected || v.lock != waiters - 1) {
        bad(address, "handoff state does not match admitted waiters");
    }
    return v;
}
void poison(const std::shared_ptr<GuestCriticalSections::Shared>& s) noexcept {
    if (!s) return;
    std::lock_guard lock(s->mutex);
    s->stopping = true;
    s->changed.notify_all();
}
void abandon(const std::shared_ptr<GuestCriticalSections::Shared>& s,
             const std::shared_ptr<GuestCriticalSections::Waiter>& waiter) noexcept {
    if (!s || !waiter) return;
    std::lock_guard lock(s->mutex);
    if (!waiter->completed && !s->stopping) {
        s->stopping = true;
        s->changed.notify_all();
    }
}
}

GuestCriticalSections::PendingEnter::PendingEnter(std::shared_ptr<Shared> shared,
                                                   std::shared_ptr<Waiter> waiter)
    : shared_(std::move(shared)), waiter_(std::move(waiter)) {}
GuestCriticalSections::PendingEnter::~PendingEnter() {
    abandon(shared_, waiter_);
}
GuestCriticalSections::PendingEnter::PendingEnter(PendingEnter&& other) noexcept
    : shared_(std::move(other.shared_)), waiter_(std::move(other.waiter_)) {}
GuestCriticalSections::PendingEnter& GuestCriticalSections::PendingEnter::operator=(PendingEnter&& other) noexcept {
    if (this != &other) {
        abandon(shared_, waiter_);
        shared_ = std::move(other.shared_);
        waiter_ = std::move(other.waiter_);
    }
    return *this;
}
bool GuestCriticalSections::PendingEnter::wait(std::stop_token stop) {
    if (!shared_ || !waiter_) return false;
    std::stop_callback callback(stop, [shared = shared_] {
        std::lock_guard lock(shared->mutex);
        shared->stopping = true;
        shared->changed.notify_all();
    });
    std::unique_lock lock(shared_->mutex);
    shared_->changed.wait(lock, [&] { return shared_->stopping || stop.stop_requested() || waiter_->selected; });
    if (shared_->stopping || stop.stop_requested()) {
        shared_->stopping = true;
        shared_->changed.notify_all();
        return false;
    }
    return !waiter_->completed;
}

GuestCriticalSections::GuestCriticalSections(GuestMemory& memory) : shared_(std::make_shared<Shared>(memory)) {}
GuestCriticalSections::~GuestCriticalSections() { stop(); }

void GuestCriticalSections::initialize(uint32_t address) {
    std::lock_guard lock(shared_->mutex);
    running(*shared_, address); preflight(*shared_, address, 0, false);
    if (shared_->active.contains(address) || queue(*shared_, address))
        bad(address, "cannot initialize an active critical section");
    initialize_critical_section(shared_->memory, address);
}
uint32_t GuestCriticalSections::initialize_and_spin_count(uint32_t address, uint32_t spin) {
    std::lock_guard lock(shared_->mutex);
    running(*shared_, address); preflight(*shared_, address, 0, false);
    if (spin > 0xffffff00u)
        throw RuntimeStop("critical-section-spin", address, "spin count conversion overflows");
    if (shared_->active.contains(address) || queue(*shared_, address))
        bad(address, "cannot initialize an active critical section");
    return initialize_critical_section_and_spin_count(shared_->memory, address, spin);
}

std::unique_ptr<GuestCriticalSections::PendingEnter> GuestCriticalSections::enter(uint32_t address,
                                                                                   uint32_t thread) {
    std::lock_guard lock(shared_->mutex);
    running(*shared_, address); preflight(*shared_, address, thread);
    const auto active = shared_->active.insert(address); // allocation before guest effects
    try {
        State v = checked(*shared_, address);
        if (v.owner == thread) {
            if (v.recursion == max_recursion || v.lock == 0x7fffffffu) bad(address, "recursion overflow");
            shared_->memory.store<uint32_t>(uint64_t(address) + 16, v.lock + 1);
            shared_->memory.store<uint32_t>(uint64_t(address) + 20, v.recursion + 1);
            return {};
        }
        if (!v.owner && v.lock == 0xffffffffu) {
            shared_->memory.store<uint32_t>(uint64_t(address) + 16, 0);
            shared_->memory.store<uint32_t>(uint64_t(address) + 20, 1);
            shared_->memory.store<uint32_t>(uint64_t(address) + 24, thread);
            return {};
        }
        if (v.lock == 0x7fffffffu) bad(address, "waiter count overflow");
        auto waiter = std::make_shared<Waiter>(Waiter{address, thread});
        // Allocate the public token before guest effects, but bind it only after
        // admission succeeds so rollback destruction cannot re-enter this mutex.
        auto result = std::unique_ptr<PendingEnter>(new PendingEnter({}, {}));
        auto [position, created] = shared_->queues.try_emplace(address);
        try { position->second.push_back(waiter); }
        catch (...) { if (created && position->second.empty()) shared_->queues.erase(position); throw; }
        try { shared_->memory.store<uint32_t>(uint64_t(address) + 16, v.lock + 1); }
        catch (...) {
            position->second.pop_back();
            if (position->second.empty()) shared_->queues.erase(position);
            waiter->completed = true;
            throw;
        }
        result->shared_ = shared_;
        result->waiter_ = waiter;
        return result;
    } catch (...) {
        if (active.second) shared_->active.erase(active.first);
        throw;
    }
}

void GuestCriticalSections::complete(PendingEnter& pending) {
    const uint32_t address = pending.waiter_ ? pending.waiter_->address : 0;
    std::lock_guard lock(shared_->mutex);
    running(*shared_, address);
    if (pending.shared_.get() != shared_.get() || !pending.waiter_) bad(address, "foreign pending token");
    auto& waiter = *pending.waiter_;
    preflight(*shared_, address, waiter.thread);
    if (waiter.completed || !waiter.selected) bad(address, "pending token is not selected");
    auto found = shared_->queues.find(address);
    if (found == shared_->queues.end() || found->second.empty() || found->second.front().get() != &waiter)
        bad(address, "pending token is not selected queue head");
    State v = checked(*shared_, address);
    if (v.owner || v.recursion) bad(address, "handoff still has an owner");
    shared_->memory.store<uint32_t>(uint64_t(address) + 20, 1);
    shared_->memory.store<uint32_t>(uint64_t(address) + 24, waiter.thread);
    waiter.completed = true;
    found->second.pop_front();
    if (found->second.empty()) shared_->queues.erase(found);
}

void GuestCriticalSections::leave(uint32_t address, uint32_t thread) {
    std::lock_guard lock(shared_->mutex);
    running(*shared_, address); preflight(*shared_, address, thread);
    State v = checked(*shared_, address);
    if (v.owner != thread || !v.recursion) bad(address, "current thread does not own critical section");
    if (v.recursion > 1) {
        shared_->memory.store<uint32_t>(uint64_t(address) + 16, v.lock - 1);
        shared_->memory.store<uint32_t>(uint64_t(address) + 20, v.recursion - 1);
        return;
    }
    shared_->memory.store<uint32_t>(uint64_t(address) + 20, 0);
    shared_->memory.store<uint32_t>(uint64_t(address) + 24, 0);
    shared_->memory.store<uint32_t>(uint64_t(address) + 16, v.lock - 1);
    auto* q = queue(*shared_, address);
    if (q && !q->empty()) {
        if (q->front()->selected) bad(address, "waiter already selected");
        q->front()->selected = true;
        shared_->changed.notify_all();
    } else {
        shared_->active.erase(address);
    }
}
void GuestCriticalSections::stop() noexcept { poison(shared_); }
}
