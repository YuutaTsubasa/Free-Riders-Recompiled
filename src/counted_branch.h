#pragma once

#include <cstdint>

namespace sfr {

inline bool branch_counter_zero_false(uint64_t& counter, bool condition) noexcept {
    --counter;
    return static_cast<uint32_t>(counter) == 0 && !condition;
}

// bdnzt: decrement CTR, branch while it is nonzero and the CR bit is set.
inline bool branch_counter_nonzero_true(uint64_t& counter, bool condition) noexcept {
    --counter;
    return static_cast<uint32_t>(counter) != 0 && condition;
}

}
