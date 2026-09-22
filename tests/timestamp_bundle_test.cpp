#include "timestamp_bundle.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t base = 0x71400000;
constexpr uint32_t uptime = base + 16;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void rejects(Operation operation, const char* category) {
    try { operation(); }
    catch (const sfr::RuntimeStop& stop) {
        require(stop.category == category, "unexpected stop category");
        return;
    }
    throw std::runtime_error(std::string("expected stop: ") + category);
}

std::vector<uint8_t> backing(const sfr::GuestMemory& memory) {
    return {memory.base() + base, memory.base() + base + 20};
}

void changing_reads_and_big_endian_bytes() {
    sfr::GuestMemory memory;
    uint32_t value = 0x12345678;
    unsigned calls = 0;
    sfr::TimestampBundle bundle(memory, [&] { ++calls; return value; });
    require(sfr::TimestampBundle::address == base, "bundle uses the diagnostic address");
    require(calls == 0, "construction does not sample uptime");
    const auto before = backing(memory);
    require(memory.load<uint32_t>(uptime) == 0x12345678, "initial uptime read");
    require(calls == 1, "word load samples once");
    value = 0x90abcdef;
    require(memory.load<uint32_t>(uptime) == value && calls == 2, "each load obtains fresh uptime");
    constexpr std::array<uint8_t, 4> expected{0x90, 0xab, 0xcd, 0xef};
    for (unsigned i = 0; i < expected.size(); ++i)
        require(memory.load<uint8_t>(uptime + i) == expected[i], "computed bytes are big endian");
    require(memory.load<uint16_t>(uptime + 1) == 0xabcd, "unaligned halfword view");
    require(calls == 7, "every scalar read samples once");
    require(backing(memory) == before, "sampling never changes backing memory");
}

void unknown_fields_stop_before_sampling() {
    sfr::GuestMemory memory;
    unsigned calls = 0;
    sfr::TimestampBundle bundle(memory, [&] { ++calls; return 7; });
    for (uint32_t offset = 0; offset < 16; ++offset) {
        rejects([&] { memory.load<uint8_t>(base + offset); }, "import-variable");
        rejects([&] { memory.store<uint8_t>(base + offset, 0xff); }, "import-variable");
    }
    rejects([&] { memory.load<uint64_t>(base + 12); }, "import-variable");
    rejects([&] { memory.load<uint16_t>(base + 15); }, "import-variable");
    for (uint32_t offset : {20u, 21u, 22u, 23u}) {
        rejects([&] { memory.load<uint8_t>(base + offset); }, "memory-access");
        rejects([&] { memory.store<uint8_t>(base + offset, 0xff); }, "memory-access");
    }
    rejects([&] { memory.load<uint8_t>(base - 1); }, "memory-access");
    rejects([&] { memory.load<uint64_t>(uptime); }, "memory-access");
    rejects([&] { memory.load<uint32_t>(uptime + 1); }, "memory-access");
    require(calls == 0, "failed and guarded reads never sample uptime");
}

void uptime_is_read_only_without_mutation() {
    sfr::GuestMemory memory;
    unsigned calls = 0;
    sfr::TimestampBundle bundle(memory, [&] { ++calls; return 0x12345678; });
    const auto before = backing(memory);
    for (uint32_t offset = 0; offset < 4; ++offset)
        rejects([&] { memory.store<uint8_t>(uptime + offset, 0xff); }, "memory-readonly");
    rejects([&] { memory.store<uint16_t>(uptime + 1, 0xffff); }, "memory-readonly");
    rejects([&] { memory.store<uint32_t>(uptime, 0xffffffff); }, "memory-readonly");
    rejects([&] { memory.store<uint64_t>(base + 12, ~uint64_t(0)); }, "import-variable");
    rejects([&] { memory.store<uint32_t>(uptime + 1, 0xffffffff); }, "memory-access");
    require(calls == 0, "writes do not sample uptime");
    require(backing(memory) == before, "all failed writes preserve backing bytes");
}

void mapping_collisions_preserve_existing_state() {
    {
        sfr::GuestMemory memory;
        memory.map(base, 20);
        memory.store<uint64_t>(base, 0x123456789abcdef0ull);
        memory.store<uint32_t>(uptime, 0xaabbccdd);
        const auto before = backing(memory);
        rejects([&] { sfr::TimestampBundle bundle(memory, [] { return 1; }); }, "memory-map");
        require(backing(memory) == before, "mapping collision preserves existing bytes");
        require(memory.load<uint64_t>(base) == 0x123456789abcdef0ull, "collision adds no guards");
        memory.store<uint32_t>(uptime, 42);
        require(memory.load<uint32_t>(uptime) == 42, "collision adds no provider");
    }
    {
        sfr::GuestMemory memory;
        sfr::TimestampBundle first(memory, [] { return 27; });
        rejects([&] { sfr::TimestampBundle second(memory, [] { return 99; }); }, "memory-map");
        require(memory.load<uint32_t>(uptime) == 27, "duplicate leaves original provider");
    }
}

void empty_provider_rejected_before_mapping() {
    sfr::GuestMemory memory;
    rejects([&] { sfr::TimestampBundle bundle(memory, {}); }, "timestamp-bundle");
    require(memory.available(base, 20), "empty provider does not reserve memory");
    sfr::TimestampBundle valid(memory, [] { return 17; });
    require(memory.load<uint32_t>(uptime) == 17, "valid construction works after empty provider");
}

void provider_failure_propagates_and_can_recover() {
    sfr::GuestMemory memory;
    bool fail = true;
    unsigned calls = 0;
    sfr::TimestampBundle bundle(memory, [&]() -> uint32_t {
        ++calls;
        if (fail) throw sfr::RuntimeStop("clock-range", uptime, "clock provider failure");
        return 0x13579bdf;
    });
    const auto before = backing(memory);
    rejects([&] { memory.load<uint32_t>(uptime); }, "clock-range");
    require(calls == 1 && backing(memory) == before, "provider failure leaves backing untouched");
    fail = false;
    require(memory.load<uint32_t>(uptime) == 0x13579bdf && calls == 2,
            "next read resamples after provider failure");
}
}

int main() {
    struct Test { const char* name; void (*run)(); };
    int failures = 0;
    for (auto test : {Test{"changing reads and byte order", changing_reads_and_big_endian_bytes},
                      Test{"unknown fields", unknown_fields_stop_before_sampling},
                      Test{"read-only uptime", uptime_is_read_only_without_mutation},
                      Test{"mapping collisions", mapping_collisions_preserve_existing_state},
                      Test{"empty provider", empty_provider_rejected_before_mapping},
                      Test{"provider failure", provider_failure_propagates_and_can_recover}}) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    if (failures) return 1;
    std::cout << "Timestamp bundle checks passed (6 groups)\n";
}
