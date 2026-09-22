#pragma once
#include "guest_memory.h"

namespace sfr {
// A bounded diagnostic implementation of the Xbox virtual allocation ABI.
// Supports decommit and whole release; no protection changes or debug/physical heaps.
class VirtualMemory {
public:
    explicit VirtualMemory(GuestMemory& memory) : memory_(memory) {}
    uint32_t allocate(uint32_t base_ptr, uint32_t size_ptr, uint32_t type,
                      uint32_t protect, uint32_t debug);
    uint32_t free(uint32_t base_ptr, uint32_t size_ptr, uint32_t type, uint32_t debug);
    uint32_t query_address_protect(uint32_t address) const;
    struct Statistics { uint64_t capacity_bytes, reserved_bytes, committed_bytes; };
    Statistics statistics() const;
    static bool overlaps_arena(uint64_t address, uint64_t size);
private:
    struct Reservation { uint32_t address, size, protect; };
    GuestMemory& memory_;
    // Sorted by address; a bounded gap walk avoids scanning every owned page.
    std::vector<Reservation> reservations_;
};
}
