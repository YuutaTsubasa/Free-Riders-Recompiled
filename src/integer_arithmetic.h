#pragma once
#include <cstdint>

namespace sfr {
void addc(uint64_t& destination, uint64_t left, uint64_t right, uint8_t& carry);
void addme(uint64_t& destination, uint64_t source, uint8_t& carry);
void subfze(uint64_t& destination, uint64_t source, uint8_t& carry);
}
