#pragma once
#include "guest_memory.h"
#include <map>
#include <mutex>

struct O1HeapInstance;

namespace sfr {
// The 4 KiB virtual view of guest physical memory used by the title startup.
// Read/write and Windows write-combined backing are supported. Other physical
// aliases, page classes and cache controls are not implemented by this
// bounded allocator.
//
// With heap set (SFR_PHYSICAL_HEAP, on by default) it is Marathon
// Recompiled's physical heap instead: the whole window is committed up front
// and o1heap hands out its pages, an allocation over-allocating by its
// alignment as Marathon's AllocPhysical does, and a free gives the block
// straight back (the renderer drops what it cached from it). Every page is
// ordinary read/write memory, as in Marathon and Unleashed; the protection
// asked for is remembered for MmQueryAddressProtect.
//
// One difference: o1heap runs over a host shadow of the window, its smallest
// block a page, not over the window itself. Its 32-byte block header would otherwise
// push every power-of-two block (most of what the title asks for) to twice
// its size, and the window cannot grow as Marathon's 1.5 GiB one can: the
// title's GPU addresses are its virtual addresses less 0xE0000000, so a
// block anywhere else would alias another's physical pages. Physical bounds
// are not honoured: the title only asks for [0, 0xffffffff]. Either way
// calls are serialized by the allocator's own mutex, not the execution permit.
class PhysicalMemory {
public:
    static constexpr uint64_t arena_base = 0xe0000000ull;
    static constexpr uint64_t arena_size = 0x1fd00000ull;
    static constexpr uint64_t physical_offset = 0x1000ull;

    explicit PhysicalMemory(GuestMemory& memory, bool heap = false);
    ~PhysicalMemory();
    PhysicalMemory(const PhysicalMemory&) = delete;
    PhysicalMemory& operator=(const PhysicalMemory&) = delete;
    // On unless SFR_PHYSICAL_HEAP=0, read once.
    static bool heap_enabled();
    bool heap() const { return heap_ != nullptr; }
    // Shadow bytes per page of the window (heap mode): o1heap's smallest
    // block, twice its alignment (64 bytes on a 64-bit host).
    static constexpr uint64_t shadow_page = sizeof(void*) * 8;
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
    // The heap's live block holding [address, address + size), or nullptr.
    struct Block { void* shadow; uint32_t size, protect; bool gpu_backed = false; };
    Block* heap_block(uint64_t address, uint64_t size);
    uint32_t heap_allocate(uint32_t size, uint32_t protect, uint32_t alignment);
    GuestMemory& memory_;
    mutable std::recursive_mutex mutex_;
    O1HeapInstance* heap_ = nullptr;
    std::unique_ptr<uint64_t[]> shadow_;  // o1heap's arena: shadow_page bytes a page
    std::map<uint32_t, Block> blocks_;  // heap mode, by aligned address
    uint64_t live_bytes_ = 0, peak_live_bytes_ = 0;  // heap mode, the pages asked for
    std::vector<Allocation> allocations_;
    std::vector<Retained> retained_;  // sorted by address, adjacent ranges merged
};
}
