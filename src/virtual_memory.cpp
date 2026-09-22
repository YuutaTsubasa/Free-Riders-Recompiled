#include "virtual_memory.h"
#include <algorithm>

namespace sfr {
namespace {
constexpr uint32_t success = 0, invalid_parameter = 0xc000000d, no_memory = 0xc0000017;
constexpr uint32_t mem_commit = 0x1000, mem_reserve = 0x2000;
constexpr uint32_t mem_top_down = 0x100000, mem_nozero = 0x800000;
constexpr uint32_t mem_large_pages = 0x20000000, mem_heap = 0x40000000;
constexpr uint32_t supported_flags = mem_commit | mem_reserve | mem_top_down |
    mem_nozero | mem_large_pages | mem_heap;
constexpr size_t reservation_budget = 4096;
struct Arena { uint32_t begin, end, page_size; };
constexpr Arena small_arena{0x10000000, 0x20000000, 0x1000};
constexpr Arena large_arena{0x40000000, 0x50000000, 0x10000};
bool in_arena(uint32_t base, Arena arena) { return base >= arena.begin && base < arena.end; }
[[noreturn]] void unsupported(uint32_t base, const char* detail) {
    throw RuntimeStop("virtual-memory-request", base, detail);
}
}

VirtualMemory::Statistics VirtualMemory::statistics() const {
    const auto small = memory_.usage(small_arena.begin, small_arena.end - small_arena.begin);
    const auto large = memory_.usage(large_arena.begin, large_arena.end - large_arena.begin);
    return {uint64_t(small_arena.end - small_arena.begin) + large_arena.end - large_arena.begin,
            small.reserved_bytes + large.reserved_bytes, small.committed_bytes + large.committed_bytes};
}

bool VirtualMemory::overlaps_arena(uint64_t address, uint64_t size) {
    if (!size || address >= GuestMemory::address_space_size || size > GuestMemory::address_space_size - address)
        throw RuntimeStop("memory-statistics", address, "invalid category range");
    for (auto arena : {small_arena, large_arena})
        if (address < arena.end && arena.begin < address + size) return true;
    return false;
}

uint32_t VirtualMemory::query_address_protect(uint32_t address) const {
    const auto reservation = std::find_if(reservations_.begin(), reservations_.end(), [&](const Reservation& r) {
        return address >= r.address && uint64_t(address) < uint64_t(r.address) + r.size;
    });
    if (reservation != reservations_.end()) return reservation->protect;
    for (auto arena : {small_arena, large_arena})
        if (in_arena(address, arena) &&
            memory_.available(address - address % arena.page_size, arena.page_size)) return 0;
    throw RuntimeStop("memory-protection", address, "address is outside allocator-owned virtual memory");
}

uint32_t VirtualMemory::free(uint32_t base_ptr, uint32_t size_ptr, uint32_t type, uint32_t debug) {
    if (!base_ptr || !size_ptr ||
        (uint64_t(base_ptr) < uint64_t(size_ptr) + 4 && uint64_t(size_ptr) < uint64_t(base_ptr) + 4))
        return invalid_parameter;
    try {
        memory_.check_write(base_ptr, 4);
        memory_.check_write(size_ptr, 4);
    } catch (const RuntimeStop& e) {
        if (e.category == "memory-access") return invalid_parameter;
        throw;
    }
    const auto base = memory_.load<uint32_t>(base_ptr);
    const auto size = memory_.load<uint32_t>(size_ptr);
    if (!base) return 0xc00000a0; // X_STATUS_MEMORY_NOT_ALLOCATED.
    if (debug) unsupported(base, "debug memory free is unsupported");
    if (type != 0x4000 && type != 0x8000) unsupported(base, "only exact MEM_DECOMMIT or MEM_RELEASE is supported");
    if (type == 0x4000 && (!size || uint64_t(base) + size > GuestMemory::address_space_size))
        return invalid_parameter;
    Arena arena = small_arena;
    if (in_arena(base, large_arena)) arena = large_arena;
    else if (!in_arena(base, small_arena)) return invalid_parameter;
    if (base % arena.page_size) unsupported(base, "unaligned free base is unsupported");
    if (type == 0x8000) {
        if (size) unsupported(base, "nonzero input size for whole release is unsupported");
        const auto owned = std::find_if(reservations_.begin(), reservations_.end(),
            [&](const Reservation& r) { return r.address == base; });
        if (owned == reservations_.end()) return 0xc0000001;
        const auto released_size = owned->size;
        for (const auto output : {base_ptr, size_ptr})
            if (uint64_t(output) < uint64_t(base) + released_size && uint64_t(base) < uint64_t(output) + 4)
                return invalid_parameter;
        auto next = reservations_;
        next.erase(next.begin() + (owned - reservations_.begin()));
        memory_.release(base, released_size);
        reservations_.swap(next);
        memory_.store<uint32_t>(base_ptr, base);
        memory_.store<uint32_t>(size_ptr, released_size);
        return success;
    }
    const uint64_t rounded = (uint64_t(size) + arena.page_size - 1) / arena.page_size * arena.page_size;
    const auto owned = std::find_if(reservations_.begin(), reservations_.end(), [&](const Reservation& r) {
        return base >= r.address && uint64_t(base) + rounded <= uint64_t(r.address) + r.size;
    });
    if (uint64_t(base) + rounded > arena.end || owned == reservations_.end()) return 0xc0000001;
    for (const auto output : {base_ptr, size_ptr})
        if (uint64_t(output) < uint64_t(base) + rounded && uint64_t(base) < uint64_t(output) + 4)
            return invalid_parameter;
    // Host failures are fatal stops, especially after discarding any bytes.
    // Retain reservation and protection metadata; only commitment changes.
    memory_.decommit(base, rounded);
    memory_.store<uint32_t>(base_ptr, base);
    memory_.store<uint32_t>(size_ptr, static_cast<uint32_t>(rounded));
    return success;
}

uint32_t VirtualMemory::allocate(uint32_t base_ptr, uint32_t size_ptr, uint32_t type,
                                 uint32_t protect, uint32_t debug) {
    // Validate both complete output spans before any allocation or output write.
    if (!base_ptr || !size_ptr ||
        (uint64_t(base_ptr) < uint64_t(size_ptr) + 4 && uint64_t(size_ptr) < uint64_t(base_ptr) + 4))
        return invalid_parameter;
    try {
        memory_.check_write(base_ptr, 4);
        memory_.check_write(size_ptr, 4);
    } catch (const RuntimeStop& e) {
        if (e.category == "memory-access") return invalid_parameter;
        throw; // Preserve unimplemented import-variable diagnostics.
    }
    const uint32_t requested_base = memory_.load<uint32_t>(base_ptr);
    const uint32_t requested_size = memory_.load<uint32_t>(size_ptr);
    if (!requested_size) return invalid_parameter;
    if (requested_size & 0x80000000u) unsupported(requested_base, "negative/high-bit region sizes are unsupported");
    if (uint64_t(requested_base) + requested_size > GuestMemory::address_space_size)
        return invalid_parameter;
    if (debug) unsupported(requested_base, "debug memory is unsupported");
    if (type & 0x80000000u) unsupported(requested_base, "MEM_16MB_PAGES is unsupported");
    if (type & 0x80000u) unsupported(requested_base, "MEM_RESET is unsupported");
    if (type & ~supported_flags) unsupported(requested_base, "unsupported allocation flags");
    if (!(type & (mem_reserve | mem_commit))) return invalid_parameter;
    if (protect != 4) unsupported(requested_base, "only PAGE_READWRITE protection is supported");

    Arena arena = type & mem_large_pages ? large_arena : small_arena;
    if (requested_base) {
        // Pinned Xenia selects fixed-address page size by heap, ignoring the flag.
        if (in_arena(requested_base, small_arena)) arena = small_arena;
        else if (in_arena(requested_base, large_arena)) arena = large_arena;
        else return invalid_parameter;
        if (requested_base % arena.page_size)
            unsupported(requested_base, "unaligned fixed base is unsupported");
    }
    const uint64_t rounded_size = (uint64_t(requested_size) + arena.page_size - 1) /
        arena.page_size * arena.page_size;
    if (rounded_size > uint64_t(arena.end) - arena.begin) return no_memory;
    const auto size = static_cast<uint32_t>(rounded_size);
    uint32_t address = requested_base;
    const bool new_reservation = !address || (type & mem_reserve);
    if (!new_reservation) {
        const auto existing = std::find_if(reservations_.begin(), reservations_.end(), [&](const Reservation& r) {
            return address >= r.address && uint64_t(address) + size <= uint64_t(r.address) + r.size;
        });
        if (existing == reservations_.end()) return no_memory;
        memory_.commit(address, size);
    } else {
        if (address && uint64_t(address) + size > arena.end) return no_memory;
        if (reservations_.size() >= reservation_budget)
            unsupported(address, "diagnostic reservation budget of 4096 exhausted");
        if (!address) {
            const bool top_down = (type & mem_top_down) != 0;
            uint64_t candidate = top_down ? uint64_t(arena.end) - size : arena.begin;
            for (;;) {
                if (top_down) {
                    for (auto it = reservations_.rbegin(); it != reservations_.rend(); ++it) {
                        const uint64_t end = uint64_t(it->address) + it->size;
                        if (candidate < end && it->address < candidate + size) {
                            if (uint64_t(it->address) < uint64_t(arena.begin) + size) return no_memory;
                            candidate = uint64_t(it->address) - size;
                        }
                    }
                } else {
                    for (const auto& r : reservations_) {
                        const uint64_t end = uint64_t(r.address) + r.size;
                        if (candidate < end && r.address < candidate + size) candidate = end;
                    }
                }
                if (candidate < arena.begin || candidate + size > arena.end) return no_memory;
                if (memory_.available(candidate, size)) {
                    address = static_cast<uint32_t>(candidate);
                    break;
                }
                // Fixed image/diagnostic maps share GuestMemory's reservation
                // registry; step around their host-rounded pages as well.
                if (top_down) {
                    if (candidate < uint64_t(arena.begin) + arena.page_size) return no_memory;
                    candidate -= arena.page_size;
                } else {
                    candidate += arena.page_size;
                }
            }
        } else if (!memory_.available(address, size)) {
            return no_memory;
        }
        // Allocate all service metadata before mutating GuestMemory. Host commit
        // failure is a diagnostic stop, never a successful partial allocation.
        auto next = reservations_;
        const auto position = std::lower_bound(next.begin(), next.end(), address,
            [](const Reservation& r, uint32_t value) { return r.address < value; });
        next.insert(position, {address, size, protect});
        memory_.reserve(address, size);
        if (type & mem_commit) memory_.commit(address, size);
        reservations_.swap(next);
    }
    // New anonymous host pages are zeroed even with MEM_NOZERO; existing pages
    // are never cleared on recommit. MEM_NOZERO makes no uninitialized guarantee.
    memory_.store<uint32_t>(base_ptr, address);
    memory_.store<uint32_t>(size_ptr, size);
    return success;
}
}
