#include "portable_waitables.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace sfr::portable {
namespace {
std::mutex& lock() {
    static std::mutex mutex;
    return mutex;
}
// Only blocked waits join this list. The shared state lock guards both the
// list and registration lifetimes, including notification and removal.
struct Waiting;
Waiting*& waiting_head() {
    static Waiting* head = nullptr;
    return head;
}
struct Waiting {
    std::condition_variable_any changed;
    std::span<Waitable* const> objects;
    Waiting* next;
    explicit Waiting(std::span<Waitable* const> values)
        : objects(values), next(waiting_head()) { waiting_head() = this; }
    Waiting(const Waiting&) = delete;
    Waiting& operator=(const Waiting&) = delete;
    ~Waiting() {
        auto** link = &waiting_head();
        while (*link != this) link = &(*link)->next;
        *link = next;
    }
};

// Called with lock() held: a waiter cannot destroy its condition variable
// while it is being notified. Wake every wait containing this object; taking
// an auto-reset event or semaphore is still serialized by the shared lock.
void notify_waiters(Waitable& object) {
    for (auto* waiter = waiting_head(); waiter; waiter = waiter->next)
        if (std::find(waiter->objects.begin(), waiter->objects.end(), &object) != waiter->objects.end())
            waiter->changed.notify_all();
}

template<class Ready, class Take>
int wait_until(std::span<Waitable* const> objects, uint32_t timeout_ms, std::stop_token stop, Ready ready, Take take) {
    std::unique_lock guard(lock());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    // Destroy before guard, so timeout/cancellation/exception cleanup remains
    // protected. Already-ready and zero-timeout waits never allocate/register.
    std::optional<Waiting> registration;
    for (;;) {
        if (stop.stop_requested()) return wait_cancelled;
        if (const int index = ready(); index >= 0) {
            take(index);
            return index;
        }
        if (timeout_ms == 0) return wait_timeout;
        if (!registration) registration.emplace(objects);
        if (timeout_ms == infinite) {
            registration->changed.wait(guard, stop, [&] { return ready() >= 0; });
        } else if (!registration->changed.wait_until(guard, stop, deadline, [&] { return ready() >= 0; })) {
            if (stop.stop_requested()) return wait_cancelled;
            return wait_timeout;
        }
    }
}
}

WaitablePtr make_event(bool manual_reset, bool initial_state) {
    auto event = std::make_shared<Waitable>(Kind::event);
    event->manual_reset = manual_reset;
    event->state = initial_state;
    return event;
}

#ifdef SFR_PORTABLE_WAITABLE_TESTING
uint32_t testing_blocked_wait_count() {
    std::lock_guard guard(lock());
    uint32_t count = 0;
    for (auto* waiter = waiting_head(); waiter; waiter = waiter->next) ++count;
    return count;
}
#endif

WaitablePtr make_semaphore(int32_t initial, int32_t maximum) {
    if (initial < 0 || maximum <= 0 || initial > maximum) throw std::invalid_argument("invalid semaphore counts");
    auto semaphore = std::make_shared<Waitable>(Kind::semaphore);
    semaphore->count = initial;
    semaphore->maximum = maximum;
    return semaphore;
}

WaitablePtr make_thread_exit() { return make_event(true, false); }

void set_event(Waitable& event) {
    std::lock_guard guard(lock());
    event.state = true;
    notify_waiters(event);
}

void reset_event(Waitable& event) {
    std::lock_guard guard(lock());
    event.state = false;
}

bool release_semaphore(Waitable& semaphore, int32_t count, int32_t* previous) {
    std::lock_guard guard(lock());
    if (count <= 0 || semaphore.count > semaphore.maximum - count) return false;
    if (previous) *previous = semaphore.count;
    semaphore.count += count;
    notify_waiters(semaphore);
    return true;
}

int wait_any(std::span<Waitable* const> objects, uint32_t timeout_ms, std::stop_token stop) {
    return wait_until(objects, timeout_ms, stop,
        [&] {
            for (size_t i = 0; i < objects.size(); ++i)
                if (objects[i]->signaled()) return int(i);
            return -1;
        },
        [&](int index) { objects[size_t(index)]->take(); });
}

int wait_all(std::span<Waitable* const> objects, uint32_t timeout_ms, std::stop_token stop) {
    return wait_until(objects, timeout_ms, stop,
        [&] {
            // The same semaphore twice needs two counts.
            for (size_t i = 0; i < objects.size(); ++i) {
                int32_t needed = 0;
                for (size_t j = 0; j < objects.size(); ++j) needed += objects[j] == objects[i];
                if (objects[i]->kind == Kind::semaphore ? objects[i]->count < needed : !objects[i]->state) return -1;
            }
            return 0;
        },
        [&](int) { for (auto* object : objects) object->take(); });
}
}
