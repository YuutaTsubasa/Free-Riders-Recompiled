#pragma once
#include "guest_memory.h"

namespace sfr {
// The 4 KiB virtual view of guest physical memory used by the title startup.
// Read/write and Windows write-combined backing are supported. Other physical
// aliases, page classes and cache controls are not implemented by this
// bounded allocator.
class PhysicalMemory {
public:
    static constexpr uint64_t arena_base = 0xe0000000ull;
    static constexpr uint64_t arena_size = 0x1fd00000ull;
    static constexpr uint64_t physical_offset = 0x1000ull;

    explicit PhysicalMemory(GuestMemory& memory);
    uint32_t allocate(uint32_t flags, uint32_t size, uint32_t protect,
                      uint32_t min, uint32_t max, uint32_t alignment);
    // Frees the allocation starting at address; returns false when none does.
    // GPU-backed memory (native resources may still refer to it) and
    // write-combined memory (the host view cannot release it) stay mapped;
    // later allocations with the same protection reuse them, zeroed, before
    // committing more (otherwise every texture the title frees would count
    // against the 512 MiB budget until it runs out).
    bool free(uint32_t address);
    // Size of the allocation starting at address, or 0.
    uint32_t allocation_size(uint32_t address) const;
    // Retained, reusable bytes (mapped but owned by no allocation).
    uint64_t retained_bytes() const;
    uint32_t query_address_protect(uint32_t address) const;
    void require_cpu_only_range(uint64_t address, uint64_t size) const;
    void mark_gpu_backed(uint64_t address, uint64_t size);
    GuestMemory::Usage statistics() const;
    static bool overlaps_arena(uint64_t address, uint64_t size);

private:
    struct Allocation { uint32_t address, size, protect; bool gpu_backed = false; };
    struct Retained { uint64_t address, size; uint32_t protect; };
    uint32_t reuse(uint64_t size, uint32_t protect, uint64_t begin, uint64_t end);
    GuestMemory& memory_;
    std::vector<Allocation> allocations_;
    std::vector<Retained> retained_;  // sorted by address, adjacent ranges merged
};
}
