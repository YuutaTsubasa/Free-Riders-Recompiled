#pragma once
#include <cstdint>
namespace sfr {
class GuestMemory;
void load_halfword_update(GuestMemory& memory, uint64_t& destination, uint64_t& base, int16_t displacement);
}
