#pragma once
#include <chrono>
#include <string>

namespace sfr {
// What configure_host_timing asked the host for, and what a short sleep then
// really took. A handheld on battery (the ROG Xbox Ally X) ran every frame in
// whole 15.625 ms timer ticks: two per race frame, 32 fps with the CPU half
// idle, because Windows ignored the process's 1 ms timer request.
struct HostTiming {
    bool throttling_opt_out = false;  // SetProcessInformation(ProcessPowerThrottling) accepted
    bool timer_period = false;        // timeBeginPeriod(1) accepted
    bool high_resolution_timer = false;
    double sleep_1ms_ms = 0;          // mean of a few std::this_thread::sleep_for(1 ms)
    double precise_1ms_ms = 0;        // mean of a few precise_sleep(1 ms)
    std::string describe() const;
};

// Once, early in the game process: keep Windows from power-throttling it
// (EcoQoS, which also moves threads to efficiency cores and ignores timer
// resolution requests) and ask for 1 ms timer resolution. No-op elsewhere.
// SFR_HOST_TIMING=0 leaves the host defaults, for comparison runs.
HostTiming configure_host_timing();

// Sleep for about `duration` without depending on the system timer tick: a
// high-resolution waitable timer on Windows, sleep_for elsewhere.
void precise_sleep(std::chrono::nanoseconds duration);
}
