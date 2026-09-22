#pragma once
#include <chrono>
#include <cstdint>

namespace sfr {
// Diagnostic monotonic profile, measured from construction without scaling or pausing.
class GuestClock {
public:
    static constexpr uint64_t frequency = 49875000;

    GuestClock();
    static uint64_t time_base_from_nanoseconds(int64_t elapsed);
    static uint32_t uptime_from_nanoseconds(int64_t elapsed);
    uint64_t time_base() const;
    uint32_t uptime_milliseconds() const;

private:
    const std::chrono::steady_clock::time_point start_;
};
}
