#pragma once
#include <chrono>
#include <string>

namespace sfr {
// What configure_host_timing asked the host for, and what a short sleep then
// really took. The Ally X capture's roughly 31 ms race frames motivate testing
// timer policy, but do not by themselves establish ignored timer requests.
struct HostTiming {
    bool throttling_opt_out = false;  // SetProcessInformation(ProcessPowerThrottling) accepted
    bool timer_resolution_opt_out = false; // IGNORE_TIMER_RESOLUTION policy accepted too
    bool timer_period = false;        // timeBeginPeriod(1) accepted
    bool high_resolution_timer = false;
    double sleep_1ms_ms = 0;          // mean of a few std::this_thread::sleep_for(1 ms)
    double precise_1ms_ms = 0;        // mean of a few precise_sleep(1 ms)
    std::string describe() const;
};

// Once, early in the game process: request execution-speed and timer-resolution
// policies independently of Windows heuristics, plus a 1 ms timer period.
// Policy changes are Windows-only; sleep measurements also run elsewhere.
// SFR_HOST_TIMING=0 leaves the host defaults, for comparison runs.
HostTiming configure_host_timing();

// Sleep for about `duration` without depending on the system timer tick: a
// high-resolution waitable timer on Windows, sleep_for elsewhere.
void precise_sleep(std::chrono::nanoseconds duration);
}
