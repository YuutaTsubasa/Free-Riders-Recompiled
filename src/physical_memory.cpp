#include "physical_memory.h"
#include <algorithm>
#include <cstring>

namespace sfr {
namespace {
constexpr uint64_t page_size = 0x1000;

[[noreturn]] void unsupported(uint64_t value, const char* detail) {
    throw RuntimeStop("physical-memory", value, detail);
}
}

PhysicalMemory::PhysicalMemory(GuestMemory& memory) : memory_(memory) {
    // GuestMemory validates this range against the actual host page size. This
    // query intentionally has no reservation or commitment side effect.
    (void)memory_.usage(arena_base, page_size);
}

GuestMemory::Usage PhysicalMemory::statistics() const {
    return memory_.usage(arena_base, arena_size);
}

bool PhysicalMemory::overlaps_arena(uint64_t address, uint64_t size) {
    if (!size || address >= GuestMemory::address_space_size ||
        size > GuestMemory::address_space_size - address)
        unsupported(address, "invalid category range");
    return address < arena_base + arena_size && arena_base < address + size;
}

bool PhysicalMemory::free(uint32_t address) {
    const auto allocation = std::find_if(allocations_.begin(), allocations_.end(),
                                         [&](const Allocation& item) { return item.address == address; });
    if (allocation == allocations_.end()) return false;
    if (!allocation->gpu_backed && allocation->protect != 0x404) {
        memory_.release(allocation->address, allocation->size);
    } else {
        Retained range{allocation->address, allocation->size, allocation->protect};
        auto at = std::lower_bound(retained_.begin(), retained_.end(), range.address,
                                   [](const Retained& r, uint64_t a) { return r.address < a; });
        at = retained_.insert(at, range);
        // Merge with neighbours of the same protection.
        if (at + 1 != retained_.end() && at->protect == (at + 1)->protect && at->address + at->size == (at + 1)->address) {
            at->size += (at + 1)->size;
            retained_.erase(at + 1);
        }
        if (at != retained_.begin() && (at - 1)->protect == at->protect && (at - 1)->address + (at - 1)->size == at->address) {
            (at - 1)->size += at->size;
            retained_.erase(at);
        }
    }
    allocations_.erase(allocation);
    return true;
}

uint32_t PhysicalMemory::allocation_size(uint32_t address) const {
    const auto allocation = std::find_if(allocations_.begin(), allocations_.end(),
                                         [&](const Allocation& item) { return item.address == address; });
    return allocation == allocations_.end() ? 0 : allocation->size;
}

uint64_t PhysicalMemory::retained_bytes() const {
    uint64_t total = 0;
    for (const auto& range : retained_) total += range.size;
    return total;
}

// The highest retained range of this protection that holds size bytes within
// the virtual window [begin, end): taken from its top, zeroed.
uint32_t PhysicalMemory::reuse(uint64_t size, uint32_t protect, uint64_t begin, uint64_t end) {
    for (size_t i = retained_.size(); i-- > 0;) {
        Retained& range = retained_[i];
        if (range.protect != protect) continue;
        const uint64_t top = std::min(range.address + range.size, end);
        const uint64_t bottom = std::max(range.address, begin);
        if (top < bottom + size) continue;
        const uint64_t address = (top - size) & ~(page_size - 1);
        if (address < bottom) continue;
        allocations_.push_back({static_cast<uint32_t>(address), static_cast<uint32_t>(size), protect});
        // Split the range around the taken pages.
        const Retained above{address + size, range.address + range.size - (address + size), protect};
        range.size = address - range.address;
        if (above.size) retained_.insert(retained_.begin() + i + 1, above);
        if (!range.size) retained_.erase(retained_.begin() + i);
        memory_.check_write(address, size);
        std::memset(memory_.base() + address, 0, size);
        return static_cast<uint32_t>(address);
    }
    return 0;
}

uint32_t PhysicalMemory::query_address_protect(uint32_t address) const {
    const auto allocation = std::find_if(allocations_.begin(), allocations_.end(), [&](const Allocation& item) {
        return address >= item.address && uint64_t(address) < uint64_t(item.address) + item.size;
    });
    if (allocation != allocations_.end()) return allocation->protect;
    if (uint64_t(address) >= arena_base && uint64_t(address) < arena_base + arena_size &&
        memory_.available(address - address % page_size, page_size))
        return 0;
    throw RuntimeStop("memory-protection", address, "address is outside allocator-owned physical memory");
}

void PhysicalMemory::require_cpu_only_range(uint64_t address, uint64_t size) const {
    if (!size || address >= GuestMemory::address_space_size ||
        size > GuestMemory::address_space_size - address)
        unsupported(address, "invalid CPU-only physical range");
    const auto allocation = std::find_if(allocations_.begin(), allocations_.end(), [&](const Allocation& item) {
        return address >= item.address && address - item.address <= item.size &&
            size <= item.size - (address - item.address);
    });
    if (allocation == allocations_.end())
        unsupported(address, "CPU-only range is not contained by one owned physical allocation");
    memory_.check_write(address, size);
    if (allocation->gpu_backed)
        unsupported(address, "physical allocation has GPU backing");
}

void PhysicalMemory::mark_gpu_backed(uint64_t address, uint64_t size) {
    if (!size || address >= GuestMemory::address_space_size ||
        size > GuestMemory::address_space_size - address)
        unsupported(address, "invalid GPU-backed physical range");
    const auto allocation = std::find_if(allocations_.begin(), allocations_.end(), [&](const Allocation& item) {
        return address >= item.address && address - item.address <= item.size &&
            size <= item.size - (address - item.address);
    });
    if (allocation == allocations_.end())
        unsupported(address, "GPU-backed range is not contained by one owned physical allocation");
    memory_.check_write(address, size);
    allocation->gpu_backed = true;
}

uint32_t PhysicalMemory::allocate(uint32_t flags, uint32_t size, uint32_t protect,
                                  uint32_t min, uint32_t max, uint32_t alignment) {
    if (flags != 0) unsupported(flags, "unsupported physical allocation flags");
    if (protect != 4 && protect != 0x404) unsupported(protect, "unsupported physical protection");
    if (alignment && (alignment > page_size || (alignment & (alignment - 1))))
        unsupported(alignment, "unsupported physical alignment");

    if (!size || min > max) return 0;
    const uint64_t adjusted_size = (uint64_t(size) + page_size - 1) & ~(page_size - 1);
    if (!adjusted_size || adjusted_size > arena_size) return 0;

    const uint64_t physical_end = physical_offset + arena_size;
    const uint64_t bounded_begin = std::max<uint64_t>(min, physical_offset);
    const uint64_t bounded_end = std::min<uint64_t>(uint64_t(max) + 1, physical_end);
    if (bounded_begin >= bounded_end || adjusted_size > bounded_end - bounded_begin) return 0;

    if (const uint32_t reused = reuse(adjusted_size, protect, arena_base + bounded_begin - physical_offset,
                                      arena_base + bounded_end - physical_offset))
        return reused;
    const auto total = memory_.usage();
    if (adjusted_size > memory_.backing_budget() - total.committed_bytes) return 0;

    uint64_t candidate = (bounded_end - adjusted_size) & ~(page_size - 1);
    if (candidate < bounded_begin) return 0;
    for (;;) {
        const uint64_t virtual_address = arena_base + candidate - physical_offset;
        if (memory_.available(virtual_address, adjusted_size)) {
            auto next = allocations_;
            next.push_back({static_cast<uint32_t>(virtual_address),
                            static_cast<uint32_t>(adjusted_size), protect});
            if (protect == 0x404)
                memory_.map_write_combined(virtual_address, adjusted_size);
            else
                memory_.map(virtual_address, adjusted_size);
            allocations_.swap(next);
            return static_cast<uint32_t>(virtual_address);
        }
        // Skip below the lowest reservation in the way instead of page by page.
        const uint64_t conflict = memory_.lowest_conflict(virtual_address, adjusted_size);
        const uint64_t below = conflict - arena_base + physical_offset;
        if (conflict < arena_base || below < adjusted_size + bounded_begin) break;
        const uint64_t next_candidate = (below - adjusted_size) & ~(page_size - 1);
        if (next_candidate >= candidate) {
            if (candidate < bounded_begin + page_size) break;
            candidate -= page_size;
        } else {
            candidate = next_candidate;
        }
    }
    return 0;
}
}
