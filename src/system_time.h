#pragma once
#include "guest_memory.h"
#include <cstdint>

namespace sfr {
// Signed 100ns ticks since 1970; dates before the FILETIME epoch (1601) stop.
uint64_t filetime_from_unix_ticks(int64_t ticks);
// Null output is a no-op. Non-null outputs receive one checked big endian qword.
void write_system_time(GuestMemory& memory, uint32_t output, int64_t ticks);
// Unscaled host wall time; no promise of monotonicity or 100ns clock resolution.
void query_system_time(GuestMemory& memory, uint32_t output);
}
