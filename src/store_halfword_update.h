#pragma once
#include <cstdint>
namespace sfr {
class GuestMemory;
void store_halfword_update(GuestMemory& memory, uint64_t& base, uint64_t source, int16_t displacement);
}
