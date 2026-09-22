#pragma once

#include <cstdint>

namespace sfr {
class GuestMemory;

void initialize_ansi_string(GuestMemory& memory, uint32_t destination, uint32_t source);
// RtlInitUnicodeString: the same counted descriptor over big-endian UTF-16
// characters, Length and MaximumLength in bytes (MaximumLength counts the
// terminator).
void initialize_unicode_string(GuestMemory& memory, uint32_t destination, uint32_t source);
// RtlUnicodeStringToAnsiString: characters above 0x7F become '?'. With
// allocate, the buffer comes from a small pool of the runtime's own (the title
// converts file names and frees them at once with RtlFreeAnsiString).
// Returns the NTSTATUS.
uint32_t unicode_string_to_ansi(GuestMemory& memory, uint32_t destination, uint32_t source, bool allocate);
// RtlFreeAnsiString: returns a pool buffer and clears the descriptor.
void free_ansi_string(GuestMemory& memory, uint32_t string);
}
