#include "host_timing.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

double mean_precise_ms(std::chrono::nanoseconds duration, int count) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) sfr::precise_sleep(duration);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / count;
}

void precise_sleep_waits_at_least_its_duration() {
    const auto start = std::chrono::steady_clock::now();
    sfr::precise_sleep(std::chrono::milliseconds(3));
    require(std::chrono::steady_clock::now() - start >= std::chrono::microseconds(2900),
            "a precise sleep must not return early");
}

void report_precise_sleep_latency() {
    // Scheduling latency is a measurement, not a portable correctness bound.
    // A loaded machine or a fallback timer may legitimately take longer.
    const double mean = mean_precise_ms(std::chrono::milliseconds(1), 20);
    std::cout << "precise_sleep(1 ms) mean " << mean << " ms\n";
    require(mean >= 0.9, "positive sleeps must not complete immediately");
}

void zero_and_negative_durations_return() {
    sfr::precise_sleep(std::chrono::nanoseconds(0));
    sfr::precise_sleep(std::chrono::nanoseconds(-5));
}

void configuration_reports_what_it_did() {
    const auto timing = sfr::configure_host_timing();
    const auto text = timing.describe();
    std::cout << text << '\n';
    require(text.rfind("HOST_TIMING ", 0) == 0, "the log line starts with its tag");
#ifdef _WIN32
    const char* setting = std::getenv("SFR_HOST_TIMING");
    const bool enabled = !setting || *setting != '0';
    require(timing.timer_period == enabled, "timer-resolution request follows the host timing option");
    if (!enabled) require(!timing.throttling_opt_out && !timing.timer_resolution_opt_out,
                          "disabled mode does not change power or timer policy");
    // High-resolution timer creation can fail; precise_sleep has a fallback.
#endif
    require(timing.precise_1ms_ms > 0.9, "the measured precise sleep lasted its duration");
}
}

int main() {
    try {
        precise_sleep_waits_at_least_its_duration();
        report_precise_sleep_latency();
        zero_and_negative_durations_return();
        configuration_reports_what_it_did();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "host_timing tests passed\n";
    return 0;
}
