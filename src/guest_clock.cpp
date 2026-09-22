#include "guest_clock.h"
#include "guest_memory.h"
#include <algorithm>
#include <limits>

namespace sfr {
GuestClock::GuestClock() : start_(std::chrono::steady_clock::now()) {}

uint64_t GuestClock::time_base_from_nanoseconds(int64_t elapsed) {
    if (elapsed < 0) throw RuntimeStop("clock-range", 0, "negative monotonic elapsed time");
    const uint64_t ns = static_cast<uint64_t>(elapsed);
    // At INT64_MAX nanoseconds both products and their sum fit uint64_t.
    // Multiplying nanoseconds directly by the frequency would overflow.
    return (ns / 1000000000) * frequency + (ns % 1000000000) * frequency / 1000000000;
}

uint32_t GuestClock::uptime_from_nanoseconds(int64_t elapsed) {
    if (elapsed < 0) throw RuntimeStop("clock-range", 0, "negative monotonic elapsed time");
    const uint64_t milliseconds = static_cast<uint64_t>(elapsed) / 1000000;
    return static_cast<uint32_t>(std::min<uint64_t>(milliseconds, std::numeric_limits<uint32_t>::max()));
}

uint64_t GuestClock::time_base() const {
    return time_base_from_nanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start_).count());
}

uint32_t GuestClock::uptime_milliseconds() const {
    return uptime_from_nanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start_).count());
}
}
