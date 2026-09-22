#include "portable_waitables.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace sfr::portable {
namespace {
std::mutex& lock() {
    static std::mutex mutex;
    return mutex;
}
std::condition_variable_any& changed() {
    static std::condition_variable_any condition;
    return condition;
}

template<class Ready, class Take>
int wait_until(uint32_t timeout_ms, std::stop_token stop, Ready ready, Take take) {
    std::unique_lock guard(lock());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (stop.stop_requested()) return wait_cancelled;
        if (const int index = ready(); index >= 0) {
            take(index);
            return index;
        }
        if (timeout_ms == 0) return wait_timeout;
        if (timeout_ms == infinite) {
            changed().wait(guard, stop, [&] { return ready() >= 0; });
        } else if (!changed().wait_until(guard, stop, deadline, [&] { return ready() >= 0; })) {
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

WaitablePtr make_semaphore(int32_t initial, int32_t maximum) {
    if (initial < 0 || maximum <= 0 || initial > maximum) throw std::invalid_argument("invalid semaphore counts");
    auto semaphore = std::make_shared<Waitable>(Kind::semaphore);
    semaphore->count = initial;
    semaphore->maximum = maximum;
    return semaphore;
}

WaitablePtr make_thread_exit() { return make_event(true, false); }

void set_event(Waitable& event) {
    {
        std::lock_guard guard(lock());
        event.state = true;
    }
    changed().notify_all();
}

void reset_event(Waitable& event) {
    std::lock_guard guard(lock());
    event.state = false;
}

bool release_semaphore(Waitable& semaphore, int32_t count, int32_t* previous) {
    {
        std::lock_guard guard(lock());
        if (count <= 0 || semaphore.count > semaphore.maximum - count) return false;
        if (previous) *previous = semaphore.count;
        semaphore.count += count;
    }
    changed().notify_all();
    return true;
}

int wait_any(std::span<Waitable* const> objects, uint32_t timeout_ms, std::stop_token stop) {
    return wait_until(timeout_ms, stop,
        [&] {
            for (size_t i = 0; i < objects.size(); ++i)
                if (objects[i]->signaled()) return int(i);
            return -1;
        },
        [&](int index) { objects[size_t(index)]->take(); });
}

int wait_all(std::span<Waitable* const> objects, uint32_t timeout_ms, std::stop_token stop) {
    return wait_until(timeout_ms, stop,
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
