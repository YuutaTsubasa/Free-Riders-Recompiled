#include "guest_clock.h"
#include <array>
#include "guest_memory.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exact_time_base_boundaries() {
    require(sfr::GuestClock::frequency == 49875000, "diagnostic frequency is 49.875 MHz");
    struct Case { int64_t ns; uint64_t ticks; };
    for (auto value : {Case{0, 0}, Case{1, 0}, Case{20, 0}, Case{21, 1},
                       Case{999999999, 49874999}, Case{1000000000, 49875000},
                       Case{1000000021, 49875001}, Case{8000000000, 399000000},
                       Case{std::numeric_limits<int64_t>::max(), 460015680338131943ull}})
        require(sfr::GuestClock::time_base_from_nanoseconds(value.ns) == value.ticks,
                "time base uses exact floored ticks without overflowing");
}

void uptime_boundaries_and_saturation() {
    struct Case { int64_t ns; uint32_t ms; };
    for (auto value : {Case{0, 0}, Case{999999, 0}, Case{1000000, 1},
                       Case{999999999, 999}, Case{1000000000, 1000},
                       Case{4294967294999999ll, 4294967294u},
                       Case{4294967295000000ll, 4294967295u},
                       Case{4294967296000000ll, 4294967295u},
                       Case{std::numeric_limits<int64_t>::max(), 4294967295u}})
        require(sfr::GuestClock::uptime_from_nanoseconds(value.ns) == value.ms,
                "uptime floors milliseconds and saturates without wrapping");
}

void rejects_negative_elapsed_time() {
    for (auto ns : std::array<int64_t, 3>{-1ll, -1000000000ll, std::numeric_limits<int64_t>::min()}) {
        for (bool time_base : {false, true}) {
            bool rejected = false;
            try {
                if (time_base) sfr::GuestClock::time_base_from_nanoseconds(ns);
                else sfr::GuestClock::uptime_from_nanoseconds(ns);
            } catch (const sfr::RuntimeStop& stop) {
                require(stop.category == "clock-range", "negative elapsed time stop category");
                rejected = true;
            }
            require(rejected, "negative elapsed time must stop");
        }
    }
}

void live_clock_progresses_from_construction() {
    const sfr::GuestClock clock;
    const auto first_ticks = clock.time_base();
    const auto host_before = std::chrono::steady_clock::now();
    const auto first_ms = clock.uptime_milliseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const auto later_ms = clock.uptime_milliseconds();
    const auto later_ticks = clock.time_base();
    const auto host_after = std::chrono::steady_clock::now();
    require(later_ticks > first_ticks && later_ms > first_ms,
            "live clock must advance monotonically");
    const auto host_ms = std::chrono::duration_cast<std::chrono::milliseconds>(host_after - host_before).count();
    require(uint64_t(later_ms - first_ms) <= uint64_t(host_ms) + 1,
            "uptime tracks elapsed host milliseconds");
    require(later_ticks >= uint64_t(later_ms) * 49875,
            "time base and uptime share the same epoch and frequency");
    const sfr::GuestClock fresh;
    const auto fresh_ticks = fresh.time_base();
    require(fresh_ticks < clock.time_base(), "new clock has its own construction epoch");
    auto previous = later_ticks;
    for (int i = 0; i < 100; ++i) {
        const auto current = clock.time_base();
        require(current >= previous, "consecutive time-base reads never decrease");
        previous = current;
    }
}
}

int main() {
    struct Test { const char* name; void (*run)(); };
    int failures = 0;
    for (auto test : {Test{"exact time base boundaries", exact_time_base_boundaries},
                      Test{"uptime boundaries and saturation", uptime_boundaries_and_saturation},
                      Test{"negative elapsed time", rejects_negative_elapsed_time},
                      Test{"live clock", live_clock_progresses_from_construction}}) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    if (failures) return 1;
    std::cout << "Guest clock checks passed (4 groups)\n";
}
