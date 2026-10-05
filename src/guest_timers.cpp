#include "guest_timers.h"

#include "native_sync_objects.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace sfr {
namespace {
using clock = std::chrono::steady_clock;
constexpr uint32_t status_success = 0, status_invalid_handle = 0xC0000008;

// FILETIME now: 100 ns units since 1601.
int64_t filetime_now() {
    const auto since_1970 = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return since_1970 + 116444736000000000ll;
}
}

struct GuestTimers::Impl {
    explicit Impl(NativeSyncObjects& o) : objects(o) {}
    struct Timer {
        bool armed = false;
        clock::time_point due{};
        std::chrono::milliseconds period{0};
    };
    NativeSyncObjects& objects;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<uint32_t, Timer> timers;
    bool stopping = false;
    std::thread scheduler;

    void run() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            clock::time_point next = clock::time_point::max();
            for (const auto& [handle, timer] : timers)
                if (timer.armed && timer.due < next) next = timer.due;
            if (next == clock::time_point::max()) {
                changed.wait(lock);
                continue;
            }
            if (changed.wait_until(lock, next) == std::cv_status::no_timeout && clock::now() < next) continue;
            const auto now = clock::now();
            for (auto& [handle, timer] : timers) {
                if (!timer.armed || timer.due > now) continue;
                objects.set_event(handle);
                if (timer.period.count()) {
                    timer.due += timer.period;
                    if (timer.due < now) timer.due = now + timer.period;
                } else {
                    timer.armed = false;
                }
            }
        }
    }
};

GuestTimers::GuestTimers(NativeSyncObjects& objects) : impl_(std::make_unique<Impl>(objects)) {
    impl_->scheduler = std::thread([this] { impl_->run(); });
}

GuestTimers::~GuestTimers() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->changed.notify_all();
    impl_->scheduler.join();
}

uint32_t GuestTimers::create(uint32_t type, uint32_t& handle) {
    const auto made = impl_->objects.create_event(type == 0, false);
    if (made.status != status_success) return made.status;
    std::lock_guard lock(impl_->mutex);
    impl_->timers[made.handle] = {};
    handle = made.handle;
    return status_success;
}

uint32_t GuestTimers::set(uint32_t handle, int64_t due, uint32_t period_ms, bool& was_set) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->timers.find(handle);
    if (found == impl_->timers.end()) return status_invalid_handle;
    was_set = found->second.armed;
    impl_->objects.reset_event(handle);
    const int64_t delay = due < 0 ? -due : (std::max<int64_t>)(0, due - filetime_now());
    found->second.armed = true;
    found->second.due = clock::now() + std::chrono::microseconds(delay / 10);
    found->second.period = std::chrono::milliseconds(period_ms);
    impl_->changed.notify_all();
    return status_success;
}

uint32_t GuestTimers::cancel(uint32_t handle, bool& was_set) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->timers.find(handle);
    if (found == impl_->timers.end()) return status_invalid_handle;
    was_set = found->second.armed;
    found->second.armed = false;
    impl_->changed.notify_all();
    return status_success;
}

bool GuestTimers::owns(uint32_t handle) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->timers.count(handle) != 0;
}

void GuestTimers::forget(uint32_t handle) {
    std::lock_guard lock(impl_->mutex);
    impl_->timers.erase(handle);
}
}
