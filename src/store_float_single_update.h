#pragma once
#include <cstdint>
namespace sfr {
class GuestMemory;
void store_float_single_update(GuestMemory& memory, uint64_t& base,
                               uint64_t source_bits, int16_t displacement);
}
