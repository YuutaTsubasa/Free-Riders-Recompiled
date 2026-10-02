#pragma once
#include <charconv>
#include <cstdint>
#include <string_view>

namespace sfr {
// Current Windows comparisons support 256; unmeasured platforms retain 32.
// A larger interval reduces calls but delays cancellation and
// urgent handoff by up to this many guest entries, not a fixed time in ms.
inline uint32_t guest_checkpoint_interval(const char* text) {
#ifdef _WIN32
    constexpr uint32_t baseline = 256;
#else
    constexpr uint32_t baseline = 32;
#endif
    if (!text || !*text) return baseline;
    const std::string_view value(text);
    uint32_t interval = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), interval);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && interval >= 1 && interval <= 4096
        ? interval : baseline;
}
}
