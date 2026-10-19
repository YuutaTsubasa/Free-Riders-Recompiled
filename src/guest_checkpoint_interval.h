#pragma once
#include <charconv>
#include <cstdint>
#include <string_view>

namespace sfr {
// 256 everywhere: measured on Windows (Issue #33) and on an AYN Thor, where
// the permit call at every 32nd entry, with its thread_local reads, was 3% of
// a race's main thread (docs/checkpoint-interval.md). A larger interval
// reduces calls but delays cancellation and urgent handoff by up to this
// many guest entries, not a fixed time in ms.
inline uint32_t guest_checkpoint_interval(const char* text) {
    constexpr uint32_t baseline = 256;
    if (!text || !*text) return baseline;
    const std::string_view value(text);
    uint32_t interval = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), interval);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && interval >= 1 && interval <= 4096
        ? interval : baseline;
}
}
