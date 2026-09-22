#include "memory_statistics.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> static void stops(F operation, const char* category) {
    try { operation(); }
    catch (const sfr::RuntimeStop& error) {
        require(error.category == category, "expected diagnostic stop category");
        return;
    }
    throw std::runtime_error("expected explicit stop");
}
struct Fixture {
    sfr::GuestMemory memory;
    sfr::VirtualMemory vm{memory};
    sfr::PhysicalMemory physical{memory};
    static constexpr sfr::MemoryRange image{0x82000000, 0x2000}, stack{0x70100000, 0x1000}, tls{0x71300000, 0x1000};
    static constexpr uint32_t output = 0x70100100;
    Fixture() {
        memory.map(image.address, image.size);
        memory.map(stack.address, stack.size);
        memory.map(tls.address, 412);
        memory.map(0x70000000, 16); // Native kernel metadata: one backing page.
    }
    uint32_t allocate(uint32_t base, uint32_t size, uint32_t flags) {
        memory.store<uint32_t>(stack.address, base);
        memory.store<uint32_t>(stack.address + 4, size);
        return vm.allocate(stack.address, stack.address + 4, flags, 4, 0);
    }
};

static void actual_accounting_and_encoding() {
    Fixture f;
    sfr::MemoryStatistics stats(f.memory, f.vm, f.physical, f.image, f.stack, f.tls);
    f.memory.store<uint32_t>(f.output, 104);
    f.memory.store<uint32_t>(f.output - 4, 0x12345678);
    f.memory.store<uint32_t>(f.output + 104, 0x87654321);
    require(stats.query(f.output) == 0, "valid query succeeds");
    std::array<uint32_t, 26> expected{};
    expected[0]=104; expected[1]=131072; expected[2]=1;
    expected[3]=131067; expected[4]=0x20000000;
    expected[8]=1; expected[9]=2; expected[11]=1; expected[25]=131071;
    for (size_t i=0; i<expected.size(); ++i)
        require(f.memory.load<uint32_t>(f.output + 4*i) == expected[i], "all 26 statistics fields reflect native backing");
    require(f.memory.base()[f.output+4] == 0 && f.memory.base()[f.output+5] == 2,
            "total pages use big endian bytes");
    require(f.memory.load<uint32_t>(f.output-4)==0x12345678 && f.memory.load<uint32_t>(f.output+104)==0x87654321,
            "query preserves output neighbors");
    require(f.allocate(0,0x100000,0x2000)==0, "reserve arena space");
    auto reserved = stats.snapshot();
    require(reserved[5]==0x100000 && reserved[3]==expected[3] && reserved[11]==1,
            "reserve-only changes virtual reservation but consumes no backing");
    require(f.allocate(0x10000000,0x2000,0x1000)==0, "commit two arena pages");
    auto committed = stats.snapshot();
    require(committed[5]==0x100000 && committed[3]==expected[3]-2 && committed[11]==3 && committed[2]==1,
            "commit consumes available backing and adds title virtual pages");
    require(f.allocate(0x10000000,0x2000,0x1000)==0, "recommit existing pages");
    require(stats.snapshot()==committed, "recommit does not double count statistics");
    require(f.allocate(0,1,0x20003000)==0, "large-page allocation");
    auto large = stats.snapshot();
    require(large[5]==0x110000 && large[3]==committed[3]-16 && large[11]==19,
            "both real allocation arenas contribute using four-KiB units");
}

static void output_failure_atomicity() {
    Fixture f;
    sfr::MemoryStatistics stats(f.memory, f.vm, f.physical, f.image, f.stack, f.tls);
    require(stats.query(0)==0xc000000d, "null output returns invalid parameter");
    for (uint32_t length : {0u,103u,105u}) {
        std::fill_n(f.memory.base()+f.output,104,0xa5);
        f.memory.store<uint32_t>(f.output,length);
        std::array<uint8_t,104> before{};
        std::copy_n(f.memory.base()+f.output,104,before.begin());
        require(stats.query(f.output)==0xc0000023, "wrong size returns buffer-too-small");
        require(std::equal(before.begin(),before.end(),f.memory.base()+f.output), "wrong size preserves full output");
    }
    stops([&]{ stats.query(0x61000000); }, "memory-access");
    f.memory.map(0x61010000,103);
    std::fill_n(f.memory.base()+0x61010000,103,0xb6);
    f.memory.store<uint32_t>(0x61010000,104);
    std::array<uint8_t,103> tail{}; std::copy_n(f.memory.base()+0x61010000,103,tail.begin());
    stops([&]{ stats.query(0x61010000); }, "memory-access");
    require(std::equal(tail.begin(),tail.end(),f.memory.base()+0x61010000), "truncated output has no partial writes");
    f.memory.store<uint32_t>(f.output,104);
    std::array<uint8_t,104> before{}; std::copy_n(f.memory.base()+f.output,104,before.begin());
    f.memory.add_read_only_word(f.output+100,[]{ return 0u; });
    stops([&]{ stats.query(f.output); }, "memory-readonly");
    require(std::equal(before.begin(),before.end(),f.memory.base()+f.output), "late read-only word prevents all output writes");
    f.memory.store<uint32_t>(0x70100300,104);
    f.memory.add_import_variable(0x70100360,"statistics guard");
    stops([&]{ stats.query(0x70100300); }, "import-variable");
    require(f.memory.load<uint32_t>(0x70100300)==104, "guarded output preserves size");
    f.memory.store<uint32_t>(0x70100500,104);
    f.memory.load_reserved_word(0x70100600);
    stops([&]{ stats.query(0x70100500); }, "reservation-interference");
    require(f.memory.load<uint32_t>(0x70100500)==104, "reservation rejection preserves size");
}

static void invalid_layouts_stop() {
    Fixture f;
    stops([&]{ sfr::MemoryStatistics bad(f.memory,f.vm,f.physical,f.image,f.image,f.tls); }, "memory-statistics");
    stops([&]{ sfr::MemoryStatistics bad(f.memory,f.vm,f.physical,{0x82000001,0x1000},f.stack,f.tls); }, "memory-statistics");
    stops([&]{ sfr::MemoryStatistics bad(f.memory,f.vm,f.physical,{0x10000000,0x1000},f.stack,f.tls); }, "memory-statistics");
    stops([&]{ sfr::MemoryStatistics bad(f.memory,f.vm,f.physical,{0x61000000,0x1000},f.stack,f.tls); }, "memory-statistics");
}
static void physical_backing_has_its_own_category() {
    Fixture f;
    sfr::MemoryStatistics stats(f.memory, f.vm, f.physical, f.image, f.stack, f.tls);
    const auto before = stats.snapshot();
    f.memory.reserve(0xe0000000, 0x3000);
    require(stats.snapshot() == before, "physical reserve alone consumes no backing or virtual capacity");
    f.memory.commit(0xe0000000, 0x2000);
    const auto after = stats.snapshot();
    require(after[6] == 2 && after[2] == before[2] && after[3] == before[3] - 2,
            "physical backing counts as title physical pages rather than kernel metadata");
    require(after[4] == before[4] && after[5] == before[5] && after[11] == before[11],
            "physical backing leaves virtual capacity and accounting unchanged");
    f.memory.commit(0xe0000000, 0x2000);
    require(stats.snapshot() == after, "physical recommit is counted once");
    stops([&]{ sfr::MemoryStatistics bad(f.memory,f.vm,f.physical,{0xe0000000,0x2000},f.stack,f.tls); }, "memory-statistics");
}
int main() {
    try {
        actual_accounting_and_encoding(); output_failure_atomicity(); invalid_layouts_stop(); physical_backing_has_its_own_category();
        std::cout << "Memory statistics checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
