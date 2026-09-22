#include "system_time.h"
#include <chrono>

namespace sfr {
uint64_t filetime_from_unix_ticks(int64_t ticks) {
    constexpr int64_t epoch_difference = 116444736000000000ll;
    if (ticks < -epoch_difference)
        throw RuntimeStop("time-range", 0, "system time precedes the 1601 FILETIME epoch");
    // Negative ticks are bounded above; nonnegative ticks use unsigned addition
    // so INT64_MAX remains valid without overflowing signed arithmetic.
    if (ticks < 0) return static_cast<uint64_t>(ticks + epoch_difference);
    return static_cast<uint64_t>(ticks) + static_cast<uint64_t>(epoch_difference);
}

void write_system_time(GuestMemory& memory, uint32_t output, int64_t ticks) {
    if (!output) return;
    const uint64_t time = filetime_from_unix_ticks(ticks);
    memory.store<uint64_t>(output, time);
}

void query_system_time(GuestMemory& memory, uint32_t output) {
    if (!output) return;
    using UnixTicks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    const auto ticks = std::chrono::floor<UnixTicks>(std::chrono::system_clock::now().time_since_epoch());
    write_system_time(memory, output, ticks.count());
}
}
