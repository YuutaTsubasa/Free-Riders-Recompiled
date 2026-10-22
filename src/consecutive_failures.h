#pragma once

#include <cstdint>

namespace sfr {

// How many failures in a row a caller skips a frame for (a window being
// dragged to a new size can refuse an acquire, a present or a resize now and
// then) before it gives up. A success resets the count.
inline constexpr uint32_t consecutive_failure_limit = 8;

// Counts one more failure; true while it is still within the limit.
[[nodiscard]] inline bool failure_within_limit(uint32_t& failures, uint32_t limit = consecutive_failure_limit) noexcept {
    return ++failures <= limit;
}

}  // namespace sfr
