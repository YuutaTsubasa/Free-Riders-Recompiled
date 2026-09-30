#include "host_timing.h"
#include <chrono>
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

void precise_sleep_is_not_a_timer_tick() {
    // The Ally X capture slept whole 15.625 ms ticks for a 1 ms request. A
    // loaded CI machine may be late now and then, never by a tick on average.
    const double mean = mean_precise_ms(std::chrono::milliseconds(1), 20);
    std::cout << "precise_sleep(1 ms) mean " << mean << " ms\n";
    require(mean < 8.0, "a 1 ms precise sleep must not round up to the 15.6 ms timer tick");
}

void zero_and_negative_durations_return() {
    const auto start = std::chrono::steady_clock::now();
    sfr::precise_sleep(std::chrono::nanoseconds(0));
    sfr::precise_sleep(std::chrono::nanoseconds(-5));
    require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50), "no sleep for no duration");
}

void configuration_reports_what_it_did() {
    const auto timing = sfr::configure_host_timing();
    const auto text = timing.describe();
    std::cout << text << '\n';
    require(text.rfind("HOST_TIMING ", 0) == 0, "the log line starts with its tag");
#ifdef _WIN32
    require(timing.timer_period, "timeBeginPeriod(1) is accepted on every supported Windows");
    require(timing.high_resolution_timer, "high-resolution waitable timers exist since Windows 10 1803");
#endif
    require(timing.precise_1ms_ms > 0.9, "the measured precise sleep lasted its duration");
}
}

int main() {
    try {
        precise_sleep_waits_at_least_its_duration();
        precise_sleep_is_not_a_timer_tick();
        zero_and_negative_durations_return();
        configuration_reports_what_it_did();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "host_timing tests passed\n";
    return 0;
}
