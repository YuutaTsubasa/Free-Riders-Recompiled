#include "vector_compare_bounds.h"

namespace sfr {

void vector_compare_bounds(const float (&value)[4], const float (&bound)[4], uint32_t (&result)[4]) {
    for (int element = 0; element < 4; ++element) {
        const float a = value[element], b = bound[element];
        // Written as the negation of the in-bounds comparison so that a NaN,
        // which compares false either way, comes out as outside both bounds.
        const uint32_t above = !(a <= b) ? 0x80000000u : 0u;
        const uint32_t below = !(a >= -b) ? 0x40000000u : 0u;
        result[element] = above | below;
    }
}

}
