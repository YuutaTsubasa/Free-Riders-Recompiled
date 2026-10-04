#include "physical_memory.h"
#include "o1heap/o1heap.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace sfr {
namespace {
constexpr uint64_t page_size = 0x1000;

[[noreturn]] void unsupported(uint64_t value, const char* detail) {
    throw RuntimeStop("physical-memory", value, detail);
}
}

PhysicalMemory::PhysicalMemory(GuestMemory& memory, bool heap) : memory_(memory) {
    // GuestMemory validates this range against the actual host page size. This
    // query intentionally has no reservation or commitment side effect.
    (void)memory_.usage(arena_base, page_size);
    if (!heap) return;
    memory_.reserve(arena_base, arena_size);
    memory_.commit(arena_base, arena_size);
    // Page n of the window is shadow bytes [n, n + 1) * shadow_page: o1heap's
    // blocks are multiples of shadow_page from its first one, and its header
    // is the first half of that, so a block's pointer names its first page.
    // The instance header takes the first pages' worth of the shadow, which
    // the window does not use.
    const size_t bytes = size_t(arena_size / page_size * shadow_page);
    shadow_ = std::make_unique<uint64_t[]>(bytes / sizeof(uint64_t));
    heap_ = o1heapInit(shadow_.get(), bytes);
    if (!heap_) unsupported(arena_base, "o1heap physical arena initialization failed");
}

PhysicalMemory::~PhysicalMemory() = default;

bool PhysicalMemory::heap_enabled() {
    static const bool on = [] { const char* text = std::getenv("SFR_PHYSICAL_HEAP"); return !text || *text != '0'; }();
    return on;
}

PhysicalMemory::Block* PhysicalMemory::heap_block(uint64_t address, uint64_t size) {
    auto at = blocks_.upper_bound(static_cast<uint32_t>(std::min<uint64_t>(address, 0xffffffffu)));
    if (at == blocks_.begin()) return nullptr;
    --at;
    if (address - at->first > at->second.size || size > at->second.size - (address - at->first)) return nullptr;
    return &at->second;
}

// As Marathon's Heap::AllocPhysical: over-allocate by the alignment and align
// up, in whole pages. Blocks are page aligned at least -- a texture's base is
// a page number -- so a free drops whole pages from the renderer's caches.
uint32_t PhysicalMemory::heap_allocate(uint32_t size, uint32_t protect, uint32_t alignment) {
    const uint64_t align = std::max<uint64_t>(alignment, page_size);
    const uint64_t rounded = (uint64_t(size) + page_size - 1) & ~(page_size - 1);
    const uint64_t pages = (rounded + align - page_size) / page_size;
    if (pages * page_size > arena_size) return 0;
    void* const shadow = o1heapAllocate(heap_, size_t(pages * shadow_page - O1HEAP_ALIGNMENT));
    if (!shadow) {
        const auto diagnostics = o1heapGetDiagnostics(heap_);
        std::cerr << "PHYSICAL_HEAP_EXHAUSTED size=" << size << " allocated=" << diagnostics.allocated / shadow_page * page_size
                  << " peak=" << diagnostics.peak_allocated / shadow_page * page_size << '\n';
        return 0;
    }
    const uint64_t page = uint64_t(static_cast<uint8_t*>(shadow) - reinterpret_cast<uint8_t*>(shadow_.get())) / shadow_page;
    const uint64_t first = arena_base + page * page_size;
    const uint32_t address = static_cast<uint32_t>((first + align - 1) & ~(align - 1));
    blocks_[address] = {shadow, static_cast<uint32_t>(rounded), protect};
    live_bytes_ += rounded;
    peak_live_bytes_ = std::max(peak_live_bytes_, live_bytes_);
    // o1heap rounds every block up to a power of two pages: say how far that goes.
    static uint64_t reported = 0;
    if (const uint64_t peak = o1heapGetDiagnostics(heap_).peak_allocated / shadow_page * page_size;
        peak >= reported + (32u << 20)) {
        reported = peak;
        std::cerr << "PHYSICAL_HEAP_PEAK bytes=" << peak << " requested=" << peak_live_bytes_ << " blocks=" << blocks_.size() << '\n';
    }
    // o1heap hands back used blocks: the console's allocation is zeroed.
    memory_.check_write(address, rounded);
    std::memset(memory_.base() + address, 0, rounded);
    return address;
}

GuestMemory::Usage PhysicalMemory::statistics() const {
    std::lock_guard lock(mutex_);
    return memory_.usage(arena_base, arena_size);
}

bool PhysicalMemory::overlaps_arena(uint64_t address, uint64_t size) {
    if (!size || address >= GuestMemory::address_space_size ||
        size > GuestMemory::address_space_size - address)
        unsupported(address, "invalid category range");
    return address < arena_base + arena_size && arena_base < address + size;
}

bool PhysicalMemory::free(uint32_t address) {
    std::lock_guard lock(mutex_);
    if (heap_) {
        const auto found = blocks_.find(address);
        if (found == blocks_.end()) return false;
        o1heapFree(heap_, found->second.shadow);
        live_bytes_ -= found->second.size;
        blocks_.erase(found);
        return true;
    }
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
    std::lock_guard lock(mutex_);
    if (heap_) {
        const auto found = blocks_.find(address);
        return found == blocks_.end() ? 0 : found->second.size;
    }
    const auto allocation = std::find_if(allocations_.begin(), allocations_.end(),
                                         [&](const Allocation& item) { return item.address == address; });
    return allocation == allocations_.end() ? 0 : allocation->size;
}

uint64_t PhysicalMemory::retained_bytes() const {
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
    if (heap_) {
        if (const Block* block = const_cast<PhysicalMemory*>(this)->heap_block(address, 1)) return block->protect;
        if (uint64_t(address) >= arena_base && uint64_t(address) < arena_base + arena_size) return 0;
        throw RuntimeStop("memory-protection", address, "address is outside allocator-owned physical memory");
    }
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
    std::lock_guard lock(mutex_);
    if (heap_) {
        const Block* block = const_cast<PhysicalMemory*>(this)->heap_block(address, size);
        if (!block) unsupported(address, "CPU-only range is not contained by one owned physical allocation");
        memory_.check_write(address, size);
        if (block->gpu_backed) unsupported(address, "physical allocation has GPU backing");
        return;
    }
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
    std::lock_guard lock(mutex_);
    if (heap_) {
        Block* block = heap_block(address, size);
        if (!block) unsupported(address, "GPU-backed range is not contained by one owned physical allocation");
        memory_.check_write(address, size);
        block->gpu_backed = true;
        return;
    }
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
    if (alignment & (alignment - 1)) unsupported(alignment, "unsupported physical alignment");
    std::lock_guard lock(mutex_);
    if (heap_) return size ? heap_allocate(size, protect, alignment) : 0;
    if (alignment > page_size) unsupported(alignment, "unsupported physical alignment");

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

    const auto address = memory_.find_available_top_down(
        arena_base + bounded_begin - physical_offset,
        arena_base + bounded_end - physical_offset, adjusted_size);
    if (!address) return 0;
    auto next = allocations_;
    next.push_back({static_cast<uint32_t>(*address), static_cast<uint32_t>(adjusted_size), protect});
    if (protect == 0x404)
        memory_.map_write_combined(*address, adjusted_size);
    else
        memory_.map(*address, adjusted_size);
    allocations_.swap(next);
    return static_cast<uint32_t>(*address);
}
}
