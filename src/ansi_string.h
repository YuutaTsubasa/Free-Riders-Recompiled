#pragma once

#include <cstdint>

namespace sfr {
class GuestMemory;

void initialize_ansi_string(GuestMemory& memory, uint32_t destination, uint32_t source);
}
