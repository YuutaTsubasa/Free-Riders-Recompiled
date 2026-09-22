#pragma once

#include <array>
#include <cstdint>

namespace sfr {

class GuestMemory;

using VectorBytes = std::array<uint8_t, 16>;

VectorBytes load_vector_memory(GuestMemory& memory, uint32_t effective_address);
VectorBytes load_vector_left(GuestMemory& memory, uint32_t effective_address);
VectorBytes load_vector_right(GuestMemory& memory, uint32_t effective_address);
void store_vector_memory(GuestMemory& memory, uint32_t effective_address, const VectorBytes& value);
void store_vector_word(GuestMemory& memory, uint32_t effective_address, const VectorBytes& value);
void store_vector_left(GuestMemory& memory, uint32_t effective_address, const VectorBytes& value);
void store_vector_right(GuestMemory& memory, uint32_t effective_address, const VectorBytes& value);

}
