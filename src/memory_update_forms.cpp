#include "memory_update_forms.h"

namespace sfr {
uint64_t single_to_double_bits(uint32_t single) {
    const uint64_t sign = static_cast<uint64_t>(single >> 31) << 63;
    const uint32_t exponent = (single >> 23) & 0xFF;
    uint64_t fraction = single & 0x7FFFFF;
    if (exponent == 0xFF)  // infinity and NaN keep their payload, unquieted as lfs does
        return sign | (uint64_t{0x7FF} << 52) | (fraction << 29);
    if (exponent != 0)
        return sign | (static_cast<uint64_t>(exponent + 1023 - 127) << 52) | (fraction << 29);
    if (fraction == 0)
        return sign;
    // Denormal single: normalize into the double's wider exponent range.
    int shift = 0;
    while (!(fraction & 0x800000)) {
        fraction <<= 1;
        ++shift;
    }
    const uint64_t biased = static_cast<uint64_t>(1 - 127 - shift + 1023);
    return sign | (biased << 52) | ((fraction & 0x7FFFFF) << 29);
}

void load_single_update(GuestMemory& memory, uint64_t& destination_bits, uint64_t& base, uint64_t offset) {
    const uint64_t effective = base + offset;
    const uint32_t single = memory.load<uint32_t>(static_cast<uint32_t>(effective));
    destination_bits = single_to_double_bits(single);
    base = effective;
}

void load_double_update(GuestMemory& memory, uint64_t& destination_bits, uint64_t& base, uint64_t offset) {
    const uint64_t effective = base + offset;
    destination_bits = memory.load<uint64_t>(static_cast<uint32_t>(effective));
    base = effective;
}
}
