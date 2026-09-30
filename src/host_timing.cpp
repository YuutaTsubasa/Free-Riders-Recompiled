#include "host_timing.h"
#include "native_timer_resolution.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace sfr {
namespace {
template <typename Sleep>
double mean_ms(int count, Sleep sleep) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) sleep();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / count;
}

#ifdef _WIN32
// One timer per thread, created on first use; null if the host has no
// high-resolution timers (before Windows 10 1803).
struct ThreadTimer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~ThreadTimer() { if (handle) CloseHandle(handle); }
};
HANDLE thread_timer() {
    thread_local ThreadTimer timer;
    return timer.handle;
}
#endif
}

std::string HostTiming::describe() const {
    std::ostringstream text;
    text << "HOST_TIMING throttling_opt_out=" << throttling_opt_out
         << " timer_resolution_opt_out=" << timer_resolution_opt_out << " timer_period=" << timer_period
         << " high_resolution_timer=" << high_resolution_timer << " sleep_1ms_ms=" << sleep_1ms_ms
         << " precise_1ms_ms=" << precise_1ms_ms;
    return text.str();
}

void precise_sleep(std::chrono::nanoseconds duration) {
    if (duration.count() <= 0) {
        std::this_thread::yield();
        return;
    }
#ifdef _WIN32
    if (HANDLE timer = thread_timer()) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<long long>(1, duration.count() / 100);  // relative, 100 ns
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
            WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
            return;
    }
#endif
    std::this_thread::sleep_for(duration);
}

HostTiming configure_host_timing() {
    HostTiming result;
    const char* setting = std::getenv("SFR_HOST_TIMING");
    const bool enabled = !setting || *setting != '0';
#ifdef _WIN32
    if (enabled) {
        // ControlMask names the policies we decide, StateMask clear says
        // "never": run at full speed, and honour our timer resolution even
        // when Windows would otherwise judge the process not worth it.
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0;
        result.throttling_opt_out = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state) != 0;
        result.timer_resolution_opt_out = result.throttling_opt_out;
        if (!result.throttling_opt_out) {
            // Older Windows 10 only knows the execution-speed policy.
            state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            result.throttling_opt_out = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state) != 0;
        }
        // Process-lifetime request paired with timeEndPeriod during teardown.
        static const NativeTimerResolution timer_resolution;
        result.timer_period = timer_resolution.active();
    }
    result.high_resolution_timer = thread_timer() != nullptr;
#endif
    result.sleep_1ms_ms = mean_ms(4, [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    result.precise_1ms_ms = mean_ms(4, [] { precise_sleep(std::chrono::milliseconds(1)); });
    return result;
}
}
