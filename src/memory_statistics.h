#pragma once
#include "guest_memory.h"
#include "virtual_memory.h"
#include "physical_memory.h"
#include <array>

namespace sfr {
struct MemoryRange { uint64_t address, size; };
class MemoryStatistics {
public:
    static constexpr uint32_t structure_size = 104;
    MemoryStatistics(GuestMemory& memory, const VirtualMemory& allocations,
                     const PhysicalMemory& physical,
                     MemoryRange image, MemoryRange stack, MemoryRange tls);
    uint32_t query(uint32_t output) const;
    std::array<uint32_t, 26> snapshot() const;
private:
    GuestMemory& memory_;
    const VirtualMemory& allocations_;
    const PhysicalMemory& physical_;
    MemoryRange image_, stack_, tls_;
};
}
