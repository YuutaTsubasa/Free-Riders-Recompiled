#pragma once
#include <cstdint>
#include <memory>

namespace sfr {
class GuestMemory;

// The title's heap (the XAPI RtlAllocateHeap family it links statically) on
// the host, as Unleashed and Marathon Recompiled do it: o1heap arenas in
// guest memory behind one mutex, so an allocation no longer runs the title's
// heap code under its critical section and the execution permit
// (docs/architecture-migration.md, phase 2). Blocks remember the heap handle
// they were asked for, so destroying a heap frees what was allocated from it,
// which the console's heap does and Marathon's does not.
class GuestHeap {
public:
    // Arenas: [0x20400000, 0x40000000) and [0x50000000, 0x70000000), clear of
    // VirtualMemory's arenas, the XAM handle at 0x20308000 and the runtime's
    // own objects from 0x70000000. Reserves and commits them.
    explicit GuestHeap(GuestMemory& memory);
    ~GuestHeap();
    GuestHeap(const GuestHeap&) = delete;
    GuestHeap& operator=(const GuestHeap&) = delete;

    // Guest address of a new block (0 when the arenas are full); zero clears it.
    uint32_t allocate(uint32_t heap, uint32_t size, bool zero);
    // False when the address is not a live block of this heap (left alone).
    bool free(uint32_t address);
    // The size asked for, or ~0u for an address that is not a live block.
    uint32_t size(uint32_t address) const;
    // As RtlReAllocateHeap: a new block holding the old contents (0 on failure,
    // the old block then kept). in_place_only fails rather than move.
    uint32_t reallocate(uint32_t heap, uint32_t address, uint32_t size, bool zero, bool in_place_only);
    bool owns(uint32_t address) const;
    // Whether an address lies in one of the arenas (read and write, always).
    static bool in_arena(uint32_t address);
    // Frees every block allocated from heap.
    void destroy(uint32_t heap);

    // SFR_HOST_HEAP=1, read once (off by default until measured).
    static bool enabled();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

extern GuestHeap* active_heap;
}
