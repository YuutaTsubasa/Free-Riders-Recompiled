#include "physical_memory.h"
#include <array>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> static void stops(F operation) {
    try { operation(); }
    catch (const sfr::RuntimeStop& error) {
        require(error.category == "physical-memory", "expected physical-memory stop category");
        return;
    }
    throw std::runtime_error("expected explicit physical-memory stop");
}

static void real_backing_rounding_and_top_down_order() {
    sfr::GuestMemory memory(0x20000);
    sfr::PhysicalMemory physical(memory);
    const uint32_t first = physical.allocate(0, 1, 4, 0, 0xffffffffu, 4);
    require(first == 0xffcff000u, "one page allocates at top of physical arena");
    require(memory.load<uint32_t>(first) == 0, "new physical backing is zeroed and readable");
    memory.store<uint32_t>(first + 4092, 0x12345678);
    require(memory.load<uint32_t>(first + 4092) == 0x12345678, "physical backing is writable");
    const uint32_t second = physical.allocate(0, 4097, 4, 0, 0xffffffffu, 0);
    require(second == first - 8192, "rounded allocations descend without overlap");
    const auto usage = physical.statistics();
    require(usage.reserved_bytes == 12288 && usage.committed_bytes == 12288,
            "statistics report actual rounded physical backing");
}

static void startup_shape_returns_reference_address() {
    sfr::GuestMemory memory;
    sfr::PhysicalMemory physical(memory);
    require(physical.allocate(0, 0x07203000, 4, 0, 0xffffffffu, 4) == 0xf8afd000u,
            "observed startup allocation has the pinned top-down address");
}

static void inclusive_physical_bounds_and_external_occupancy() {
    sfr::GuestMemory memory(0x10000);
    sfr::PhysicalMemory physical(memory);
    require(physical.allocate(0, 1, 4, 0x2000, 0x4fff, 4) == 0xe0003000u,
            "inclusive physical maximum admits the last complete page");
    memory.map(0xe0002000, 4096);
    require(physical.allocate(0, 1, 4, 0x1000, 0x3fff, 4) == 0xe0001000u,
            "scan observes externally occupied guest ranges");
    require(memory.available(0xe0000000, 4096), "allocations stay within their physical bounds");
}

static void ordinary_failures_preserve_memory() {
    sfr::GuestMemory memory(0x4000);
    memory.map(0x70000000, 0x2000);
    sfr::PhysicalMemory physical(memory);
    const auto before = physical.statistics();
    require(physical.allocate(0, 0x3000, 4, 0, 0xffffffffu, 4) == 0,
            "backing-budget exhaustion returns null");
    require(physical.statistics().reserved_bytes == before.reserved_bytes &&
            physical.statistics().committed_bytes == before.committed_bytes,
            "budget failure leaves arena metadata unchanged");
    require(memory.available(sfr::PhysicalMemory::arena_base, 4096),
            "budget failure leaves guest pages available");
    require(physical.query_address_protect(static_cast<uint32_t>(sfr::PhysicalMemory::arena_base)) == 0,
            "failed allocation publishes no physical protection metadata");

    require(physical.allocate(0, 0, 4, 0, 0xffffffffu, 4) == 0, "zero size returns null");
    require(physical.allocate(0, 0xffffffffu, 4, 0, 0xffffffffu, 4) == 0,
            "rounding overflow or oversized size returns null");
    require(physical.allocate(0, 1, 4, 9, 8, 4) == 0, "reversed bounds return null");
    require(physical.allocate(0, 1, 4, 0x1001, 0x2000, 4) == 0,
            "bounds with enough bytes but no complete aligned page return null");
    require(physical.allocate(0, 1, 4, 0x1fd01000, 0xffffffffu, 4) == 0,
            "bounds beyond known physical memory return null");

    memory.map(sfr::PhysicalMemory::arena_base, 4096);
    const auto occupied = physical.statistics();
    require(physical.allocate(0, 1, 4, 0x1000, 0x1fff, 4) == 0,
            "occupied constrained range returns null");
    require(physical.statistics().reserved_bytes == occupied.reserved_bytes &&
            physical.statistics().committed_bytes == occupied.committed_bytes,
            "space exhaustion leaves metadata unchanged");
}

static void unsupported_requests_stop_before_mutation() {
    sfr::GuestMemory memory(0x10000);
    sfr::PhysicalMemory physical(memory);
    for (uint32_t flags : {1u, 2u, 0x80000000u})
        stops([&] { physical.allocate(flags, 1, 4, 0, 0xffffffffu, 4); });
    for (uint32_t protect : {0u, 1u, 2u, 8u, 0x104u})
        stops([&] { physical.allocate(0, 1, protect, 0, 0xffffffffu, 4); });
    for (uint32_t alignment : {3u, 8192u, 0x80000000u})
        stops([&] { physical.allocate(0, 1, 4, 0, 0xffffffffu, alignment); });
    require(physical.statistics().reserved_bytes == 0 && physical.statistics().committed_bytes == 0,
            "unsupported requests do not mutate memory");
}

static void overlap_queries_are_checked() {
    using P = sfr::PhysicalMemory;
    require(P::overlaps_arena(P::arena_base, 1), "arena beginning overlaps");
    require(P::overlaps_arena(P::arena_base + P::arena_size - 1, 1), "arena last byte overlaps");
    require(!P::overlaps_arena(P::arena_base - 4096, 4096), "range immediately below does not overlap");
    require(!P::overlaps_arena(P::arena_base + P::arena_size, 4096), "range immediately above does not overlap");
    stops([] { sfr::PhysicalMemory::overlaps_arena(0, 0); });
    stops([] { sfr::PhysicalMemory::overlaps_arena(0xffffffffu, 2); });
    stops([] { sfr::PhysicalMemory::overlaps_arena(sfr::GuestMemory::address_space_size, 1); });
}

static void write_combined_backing_is_native() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // Off by default: the renderer reads the title's GPU buffers on the CPU.
    {
        sfr::GuestMemory cached(0x10000);
        sfr::PhysicalMemory plain(cached);
        const auto combined = plain.allocate(0, 0x12c0, 0x404, 0, 0xffffffffu, 0x20);
        MEMORY_BASIC_INFORMATION information{};
        require(VirtualQuery(cached.base() + combined, &information, sizeof(information)) == sizeof(information) &&
                information.State == MEM_COMMIT && information.Protect == PAGE_READWRITE,
                "a physical write-combine request is cached by default");
    }
    sfr::GuestMemory::set_host_write_combining(true);
    struct Restore { ~Restore() { sfr::GuestMemory::set_host_write_combining(false); } } restore;
    sfr::GuestMemory memory(0x10000);
    sfr::PhysicalMemory physical(memory);
    const auto normal = physical.allocate(0, 1, 4, 0, 0xffffffffu, 4);
    const auto combined = physical.allocate(0, 0x12c0, 0x404, 0, 0xffffffffu, 0x20);
    require(combined == normal - 0x2000, "write-combined request keeps physical rounding and placement");
    MEMORY_BASIC_INFORMATION information{};
    require(VirtualQuery(memory.base() + combined, &information, sizeof(information)) == sizeof(information) &&
            information.State == MEM_COMMIT && information.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE),
            "physical write-combine request uses actual Windows write-combined pages");
    require(memory.load<uint32_t>(combined) == 0, "write-combined backing starts zeroed");
    memory.store<uint64_t>(combined + 0x1ff8, 0x123456789abcdef0ull);
    require(memory.load<uint64_t>(combined + 0x1ff8) == 0x123456789abcdef0ull,
            "write-combined physical memory is readable and writable");
    require(physical.statistics().committed_bytes == 0x3000, "both cache classes count actual backing once");
#endif
}

static void protection_queries_use_owned_allocations() {
    sfr::GuestMemory memory(0x10000);
    sfr::PhysicalMemory physical(memory);
    const uint32_t free_page = static_cast<uint32_t>(sfr::PhysicalMemory::arena_base);
    const auto before = physical.statistics();
    require(physical.query_address_protect(free_page) == 0, "free page in physical arena has no protection");
    require(physical.query_address_protect(free_page + 1) == 0 &&
            physical.query_address_protect(free_page + 0xfff) == 0, "unaligned free physical query uses its page");
    const auto after_free = physical.statistics();
    require(after_free.reserved_bytes == before.reserved_bytes &&
            after_free.committed_bytes == before.committed_bytes,
            "free physical protection query does not allocate backing");

    const uint32_t normal = physical.allocate(0, 1, 4, 0, 0xffffffffu, 4);
    memory.store<uint8_t>(normal, 0x5a);
    require(physical.query_address_protect(normal) == 4 &&
            physical.query_address_protect(normal + 0xfff) == 4,
            "normal physical allocation reports protection over its rounded page");
    require(memory.load<uint8_t>(normal) == 0x5a, "physical protection query preserves committed bytes");
    require(physical.query_address_protect(normal - 0x1000) == 0,
            "free physical hole adjacent to allocation reports zero");

    memory.map(free_page, 1);
    bool foreign_stopped = false;
    try { (void)physical.query_address_protect(free_page); }
    catch (const sfr::RuntimeStop& error) { foreign_stopped = error.category == "memory-protection"; }
    require(foreign_stopped, "foreign physical mapping stops instead of guessing protection");
    bool unknown_stopped = false;
    try { (void)physical.query_address_protect(0x1000); }
    catch (const sfr::RuntimeStop& error) { unknown_stopped = error.category == "memory-protection"; }
    require(unknown_stopped, "address outside physical arena stops explicitly");

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const uint32_t combined = physical.allocate(0, 1, 0x404, 0, 0xffffffffu, 4);
    require(physical.query_address_protect(combined) == 0x404,
            "write-combined physical allocation retains its guest protection value");
#endif
    const auto after = physical.statistics();
    require(after.reserved_bytes == after.committed_bytes,
            "physical protection queries do not reserve or commit extra pages");
}

static void cpu_only_ownership_is_bounded_and_gpu_marking_is_conservative() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(0x10000);
    sfr::PhysicalMemory physical(memory);
    const uint32_t first = physical.allocate(0, page, 4, 0, 0xffffffffu, 4);
    const uint32_t second = physical.allocate(0, page, 4, 0, 0xffffffffu, 4);
    require(first == second + page, "CPU ownership fixture allocations are adjacent");

    physical.require_cpu_only_range(first + 7, 19);
    physical.require_cpu_only_range(first + page - 1, 1);
    physical.require_cpu_only_range(second, page);

    for (const auto& range : std::array<std::array<uint64_t, 2>, 4>{{
             {{first, 0}}, {{0xffffffffull, 2}}, {{0x100000000ull, 1}}, {{uint64_t(first) + page, 1}},
         }}) {
        bool rejected = false;
        try { physical.require_cpu_only_range(range[0], range[1]); }
        catch (const sfr::RuntimeStop& error) { rejected = error.category == "physical-memory"; }
        require(rejected, "invalid or outside CPU-only range is rejected explicitly");
        rejected = false;
        try { physical.mark_gpu_backed(range[0], range[1]); }
        catch (const sfr::RuntimeStop& error) { rejected = error.category == "physical-memory"; }
        require(rejected, "invalid or outside GPU mark range is rejected explicitly");
    }

    bool cross_rejected = false;
    try { physical.mark_gpu_backed(second + page - 16, 32); }
    catch (const sfr::RuntimeStop& error) { cross_rejected = error.category == "physical-memory"; }
    require(cross_rejected, "range spanning adjacent allocations cannot be marked");
    physical.require_cpu_only_range(first, 1);
    physical.require_cpu_only_range(second, 1);

    const uint32_t foreign = static_cast<uint32_t>(sfr::PhysicalMemory::arena_base);
    memory.map(foreign, page);
    memory.reserve(foreign + page, page);
    for (uint32_t address : {foreign, foreign + static_cast<uint32_t>(page)}) {
        bool rejected = false;
        try { physical.require_cpu_only_range(address, 1); }
        catch (const sfr::RuntimeStop& error) { rejected = error.category == "physical-memory"; }
        require(rejected, "foreign mapped or reserved physical range is not allocator-owned");
        rejected = false;
        try { physical.mark_gpu_backed(address, 1); }
        catch (const sfr::RuntimeStop& error) { rejected = error.category == "physical-memory"; }
        require(rejected, "foreign mapped or reserved physical range cannot be GPU-marked");
    }
    physical.require_cpu_only_range(first, 1);
    physical.require_cpu_only_range(second, 1);

    memory.store<uint32_t>(first, 0x12345678);
    const uint32_t protection = physical.query_address_protect(first);
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION before{}, after{};
    require(VirtualQuery(memory.base() + first, &before, sizeof(before)) == sizeof(before),
            "GPU mark fixture native protection is queryable");
#endif
    physical.mark_gpu_backed(first + 8, 1);
    bool whole_rejected = false;
    try { physical.require_cpu_only_range(first, page); }
    catch (const sfr::RuntimeStop& error) { whole_rejected = error.category == "physical-memory"; }
    require(whole_rejected, "partial GPU mark makes the whole containing allocation non-CPU-only");
    whole_rejected = false;
    try { physical.require_cpu_only_range(first + page - 1, 1); }
    catch (const sfr::RuntimeStop& error) { whole_rejected = error.category == "physical-memory"; }
    require(whole_rejected, "GPU-backed state applies through the allocation's rounded tail");
    physical.require_cpu_only_range(second, page);
    require(memory.load<uint32_t>(first) == 0x12345678 &&
            physical.query_address_protect(first) == protection,
            "GPU marking changes neither physical bytes nor guest protection metadata");
#ifdef _WIN32
    require(VirtualQuery(memory.base() + first, &after, sizeof(after)) == sizeof(after) &&
            after.Protect == before.Protect,
            "GPU marking preserves native page protection");
#endif
}

static void freeing_releases_cpu_memory_and_keeps_gpu_memory() {
    sfr::GuestMemory memory(0x20000);
    sfr::PhysicalMemory physical(memory);
    const uint32_t cpu = physical.allocate(0, 4096, 4, 0, 0xffffffffu, 0);
    const uint32_t gpu = physical.allocate(0, 4096, 4, 0, 0xffffffffu, 0);
    physical.mark_gpu_backed(gpu, 4096);
    require(!physical.free(cpu + 4), "only an allocation base can be freed");
    require(physical.free(cpu) && memory.available(cpu, 4096), "freed CPU memory is released");
    require(!physical.free(cpu), "a freed allocation is forgotten");
    require(physical.allocate(0, 4096, 4, 0, 0xffffffffu, 0) == cpu, "released memory is reused");
    require(physical.free(gpu) && !memory.available(gpu, 4096), "GPU-backed memory stays mapped");
    require(!physical.free(gpu), "a freed GPU allocation is forgotten");
    const uint32_t combined = physical.allocate(0, 4096, 0x404, 0, 0xffffffffu, 0);
    require(physical.free(combined) && !memory.available(combined, 4096), "write-combined memory stays mapped");
}

static void retained_memory_is_reused_without_new_backing() {
    sfr::GuestMemory memory(0x10000);
    sfr::PhysicalMemory physical(memory);
    const uint32_t first = physical.allocate(0, 0x3000, 0x404, 0, 0xffffffffu, 0);
    memory.store<uint32_t>(first + 0x2000, 0x12345678);
    const auto before = physical.statistics();
    require(physical.free(first) && physical.retained_bytes() == 0x3000, "freed write-combined pages are retained");
    const uint32_t part = physical.allocate(0, 0x1000, 0x404, 0, 0xffffffffu, 0);
    require(part == first + 0x2000 && memory.load<uint32_t>(part) == 0,
            "reuse takes the top of the retained range, zeroed");
    require(physical.statistics().committed_bytes == before.committed_bytes, "reuse commits nothing new");
    require(physical.allocate(0, 0x1000, 4, 0, 0xffffffffu, 0) != first + 0x1000,
            "another protection does not take write-combined pages");
    require(physical.free(part) && physical.retained_bytes() == 0x3000, "adjacent retained ranges merge");
    require(physical.allocate(0, 0x3000, 0x404, 0, 0xffffffffu, 0) == first, "merged range serves a larger request");
    // Repeated allocate/free cycles no longer grow the backing.
    for (int i = 0; i < 64; ++i) {
        const uint32_t cycle = physical.allocate(0, 0x2000, 0x404, 0, 0xffffffffu, 0);
        require(cycle != 0 && physical.free(cycle), "allocation cycle stays within the budget");
    }
}

int main() {
    try {
        freeing_releases_cpu_memory_and_keeps_gpu_memory();
        retained_memory_is_reused_without_new_backing();
        real_backing_rounding_and_top_down_order();
        startup_shape_returns_reference_address();
        inclusive_physical_bounds_and_external_occupancy();
        ordinary_failures_preserve_memory();
        unsupported_requests_stop_before_mutation();
        overlap_queries_are_checked();
        write_combined_backing_is_native();
        protection_queries_use_owned_allocations();
        cpu_only_ownership_is_bounded_and_gpu_marking_is_conservative();
        std::cout << "Physical memory checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
