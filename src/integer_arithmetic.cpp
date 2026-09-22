#include "integer_arithmetic.h"
#include "guest_memory.h"

namespace sfr {
void addc(uint64_t& destination, uint64_t left, uint64_t right, uint8_t& carry) {
    const uint64_t low_sum = uint64_t(uint32_t(left)) + uint32_t(right);
    destination = left + right;
    carry = low_sum > UINT32_MAX;
}

void addme(uint64_t& destination, uint64_t source, uint8_t& carry) {
    if (carry > 1)
        throw RuntimeStop("arithmetic-carry", 0, "addme requires a one-bit carry value");
    const uint8_t old_carry = carry;
    destination = source + uint64_t(old_carry) - 1;
    carry = old_carry || uint32_t(source) != 0;
}

void subfze(uint64_t& destination, uint64_t source, uint8_t& carry) {
    if (carry > 1)
        throw RuntimeStop("arithmetic-carry", 0, "subfze requires a one-bit carry value");
    const uint8_t old_carry = carry;
    // Xbox's 32-bit carry model with a full 64-bit GPR result. Unsigned
    // arithmetic defines wraparound even for the signed minimum operand.
    destination = ~source + uint64_t(old_carry);
    carry = old_carry && uint32_t(source) == 0;
}
}
