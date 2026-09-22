#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace sfr {
class GuestMemory;

// Formats a guest printf format string. next_argument() yields each 64-bit
// argument slot in order (integers, pointers and doubles each take one slot).
// Supports %d %i %u %x %X %o %c %s %p %f %F %e %E %g %G %% with flags, width,
// precision ('*' too) and the h, hh, l, ll, I64 and z length modifiers.
std::string guest_format(GuestMemory& memory, uint32_t format, const std::function<uint64_t()>& next_argument);

// Microsoft _snprintf/_vsnprintf semantics: writes at most count bytes, adds
// a terminator only when it fits, returns the length or -1 when truncated.
int32_t guest_store_limited(GuestMemory& memory, uint32_t buffer, uint32_t count, const std::string& text);
// sprintf/vsprintf: writes the text and its terminator, returns the length.
int32_t guest_store(GuestMemory& memory, uint32_t buffer, const std::string& text);
}
