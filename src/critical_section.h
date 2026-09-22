#pragma once

#include "guest_memory.h"

#include <cstdint>

namespace sfr {
void initialize_critical_section(GuestMemory& memory, uint32_t address);
uint32_t initialize_critical_section_and_spin_count(GuestMemory& memory, uint32_t address,
                                                    uint32_t spin_count);
// Bounded single-guest-thread transitions. The caller supplies the current guest
// thread object; contention and unsupported states stop instead of waiting.
void enter_critical_section(GuestMemory& memory, uint32_t address, uint32_t thread);
void leave_critical_section(GuestMemory& memory, uint32_t address, uint32_t thread);
}
