#pragma once
#include <cstdint>
#include <span>

namespace sfr {
// Convert big-endian 32-bit words in place or into a separate buffer. The
// destination may be write-combined GPU memory and is only written, never read.
void swap_words(std::span<uint8_t> bytes);
void swap_words_into(std::span<uint8_t> to, std::span<const uint8_t> from);
}
