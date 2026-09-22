#pragma once

#include <cstdint>

namespace sfr {
class GuestMemory;

void initialize_ansi_string(GuestMemory& memory, uint32_t destination, uint32_t source);
// RtlInitUnicodeString: the same counted descriptor over big-endian UTF-16
// characters, Length and MaximumLength in bytes (MaximumLength counts the
// terminator).
void initialize_unicode_string(GuestMemory& memory, uint32_t destination, uint32_t source);
}
