#include "thread_local_storage.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using sfr::GuestMemory;
using sfr::ThreadLocalStorage;
static constexpr uint32_t base = ThreadLocalStorage::static_address;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
static void rejects(Operation operation, const char* category) {
    try { operation(); }
    catch (const sfr::RuntimeStop& stop) {
        require(stop.category == category, "unexpected stop category");
        return;
    }
    throw std::runtime_error(std::string("expected stop: ") + category);
}

// Inspect backing bytes even after a guard is installed to verify no partial writes.
static std::vector<uint8_t> bytes(const GuestMemory& memory, uint32_t address, uint32_t size) {
    return {memory.base() + address, memory.base() + address + size};
}

static void allocation_and_coherence() {
    GuestMemory memory;
    ThreadLocalStorage tls(memory, 3, 0, 0, 0);
    require(tls.allocate() == 0, "first allocation must return slot zero");
    require(tls.allocate() == 1 && tls.allocate() == 2, "multiple slots in ascending order");
    require(tls.set(0, 0xabcdef01) == 1 && tls.get(0) == 0xabcdef01, "set/get roundtrip");
    require(bytes(memory, base, 4) == std::vector<uint8_t>({0xab, 0xcd, 0xef, 1}), "set writes guest big endian bytes");
    memory.store<uint32_t>(base + 4, 0x12345678);
    require(tls.get(1) == 0x12345678, "get sees direct guest writes");
    require(tls.free(1) == 1 && memory.load<uint32_t>(base + 4) == 0, "free zeroes guest word");
    require(tls.free(0) == 1, "free first slot");
    memory.store<uint32_t>(base, 0xffffffff);
    require(tls.allocate() == 0 && tls.get(0) == 0, "lowest freed slot reused and zeroed");
    require(tls.allocate() == 1, "next freed slot reused");
}

static void initialization_and_exact_bounds() {
    GuestMemory memory;
    memory.map(0x10000, 5);
    const std::vector<uint8_t> raw{0x80, 0, 0xff, 0x12, 0x34};
    for (uint32_t i = 0; i < raw.size(); ++i) memory.store<uint8_t>(0x10000 + i, raw[i]);
    ThreadLocalStorage tls(memory, 2, 0x10000, 7, 5);
    require(tls.dynamic_address() == base + 7, "dynamic slots follow exact static size");
    memory.check(base, 15);
    require(bytes(memory, base, 5) == raw, "copy raw bytes exactly");
    require(bytes(memory, base + 5, 10) == std::vector<uint8_t>(10), "zero static padding and all dynamic bytes");
    memory.check(base, 15);
    rejects([&] { memory.check(base - 1, 1); }, "memory-access");
    // Committed in whole pages (a partly committed page takes the checked
    // path); the rest of the page is zero.
    require(bytes(memory, base + 15, 4096 - 15) == std::vector<uint8_t>(4096 - 15), "page tail is zero");
    rejects([&] { memory.check(base + 4096, 1); }, "memory-access");
    require(tls.allocate() == 0 && tls.set(0, 0x12345678) == 1, "unaligned dynamic word works");
    require(bytes(memory, base + 7, 4) == std::vector<uint8_t>({0x12, 0x34, 0x56, 0x78}), "unaligned word is big endian");
    require(bytes(memory, 0x10000, 5) == raw, "source bytes preserved");
}

static void invalid_indices_and_capacity() {
    GuestMemory memory;
    ThreadLocalStorage tls(memory, 2, 0, 0, 0);
    require(tls.free(0xffffffff) == 0, "sentinel free returns zero");
    for (uint32_t index : {0u, 1u, 2u, 0xfffffffeu, 0xffffffffu}) {
        rejects([&] { tls.get(index); }, "tls-index");
        rejects([&] { tls.set(index, 1); }, "tls-index");
        if (index != 0xffffffff) rejects([&] { tls.free(index); }, "tls-index");
    }
    require(tls.allocate() == 0 && tls.allocate() == 1, "invalid operations preserve free ownership");
    tls.set(0, 0x12345678);
    tls.set(1, 0xabcdef01);
    auto before = bytes(memory, base, 8);
    rejects([&] { tls.allocate(); }, "tls-capacity");
    rejects([&] { tls.set(2, 0); }, "tls-index");
    rejects([&] { tls.free(2); }, "tls-index");
    require(tls.free(0xffffffff) == 0, "sentinel free while full returns zero");
    require(bytes(memory, base, 8) == before, "capacity and invalid operations preserve bytes");
    require(tls.get(0) == 0x12345678 && tls.get(1) == 0xabcdef01, "capacity preserves allocated ownership");
    tls.free(0);
    before = bytes(memory, base, 8);
    rejects([&] { tls.free(0); }, "tls-index");
    rejects([&] { tls.get(0); }, "tls-index");
    rejects([&] { tls.set(0, 1); }, "tls-index");
    require(bytes(memory, base, 8) == before && tls.allocate() == 0, "double free preserves bytes and ownership");
}

static void metadata_rejection() {
    struct Config { uint32_t slots, size, raw; };
    for (auto config : {Config{0, 0, 0}, Config{2049, 0, 0}, Config{0xffffffff, 0, 0},
                        Config{1, 65537, 0}, Config{1, 0xffffffff, 0},
                        Config{1, 4, 5}, Config{1, 4, 0xffffffff}}) {
        GuestMemory memory;
        rejects([&] { ThreadLocalStorage tls(memory, config.slots, 0, config.size, config.raw); }, "tls-config");
        require(memory.available(base, 4), "invalid metadata must not reserve destination");
    }
    GuestMemory memory;
    ThreadLocalStorage tls(memory, 2048, 0xffffffff, 65536, 0);
    memory.check(base, 65536 + 2048 * 4);
    rejects([&] { memory.check(base + 65536 + 2048 * 4, 1); }, "memory-access");
    require(bytes(memory, base, 65536 + 2048 * 4) == std::vector<uint8_t>(65536 + 2048 * 4), "maximum supported storage zeroed");
    for (uint32_t i = 0; i < 2048; ++i) require(tls.allocate() == i, "all supported slots allocatable");
    rejects([&] { tls.allocate(); }, "tls-capacity");
}

static void source_and_destination_failures() {
    for (uint32_t address : {0u, 0xfffffffeu}) {
        GuestMemory memory;
        rejects([&] { ThreadLocalStorage tls(memory, 1, address, 4, 4); }, "memory-access");
        require(memory.available(base, 8), "invalid or wrapping source checked before destination mapping");
    }
    {
        GuestMemory memory;
        memory.map(0x10000, 3);
        memory.store<uint8_t>(0x10000, 0x5a);
        rejects([&] { ThreadLocalStorage tls(memory, 1, 0x10000, 4, 4); }, "memory-access");
        require(memory.available(base, 8) && memory.load<uint8_t>(0x10000) == 0x5a, "partial source failure preserves memory");
    }
    {
        GuestMemory memory;
        memory.map(0x10000, 8);
        memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
        auto before = bytes(memory, 0x10000, 8);
        memory.add_import_variable(0x10004, "guarded raw TLS");
        rejects([&] { ThreadLocalStorage tls(memory, 1, 0x10000, 8, 8); }, "import-variable");
        require(memory.available(base, 12) && bytes(memory, 0x10000, 8) == before, "source guard checked before mapping or mutation");
    }
    {
        GuestMemory memory;
        memory.map(base, 8);
        memory.store<uint64_t>(base, 0x123456789abcdef0ull);
        auto before = bytes(memory, base, 8);
        rejects([&] { ThreadLocalStorage tls(memory, 1, base, 4, 4); }, "memory-map");
        require(bytes(memory, base, 8) == before, "destination collision preserves existing bytes");
    }
}

static void guard_failures_preserve_ownership() {
    {
        GuestMemory memory;
        ThreadLocalStorage tls(memory, 2, 0, 0, 0);
        memory.store<uint32_t>(base, 0x12345678);
        auto before = bytes(memory, base, 8);
        memory.add_import_variable(base + 2, "partial allocation guard");
        rejects([&] { tls.allocate(); }, "import-variable");
        rejects([&] { tls.get(0); }, "tls-index");
        rejects([&] { tls.allocate(); }, "import-variable");
        require(bytes(memory, base, 8) == before, "failed allocation preserves bytes and free ownership");
    }
    {
        GuestMemory memory;
        ThreadLocalStorage tls(memory, 2, 0, 0, 0);
        tls.allocate();
        tls.allocate();
        tls.set(0, 0x12345678);
        tls.set(1, 0xabcdef01);
        memory.check(base, 8);
        auto before = bytes(memory, base, 8);
        memory.add_import_variable(base + 2, "partial live slot guard");
        rejects([&] { tls.set(0, 0); }, "import-variable");
        rejects([&] { tls.get(0); }, "import-variable");
        rejects([&] { tls.free(0); }, "import-variable");
        rejects([&] { tls.free(0); }, "import-variable");
        rejects([&] { tls.allocate(); }, "tls-capacity");
        require(bytes(memory, base, 8) == before, "failed free/set preserve all bytes and allocated ownership");
    }
}

static void independent_banks_share_allocation() {
    GuestMemory memory;
    ThreadLocalStorage tls(memory, 2, 0, 0, 0);
    constexpr uint32_t worker = 0x73002000, next = 0x73202000;
    memory.map(worker, 8); memory.map(next, 8);
    tls.allocate(); tls.set(0, 0x11223344);
    memory.store<uint32_t>(worker, 0xFFFFFFFF);
    tls.register_thread(worker);
    require(tls.get_for(worker, 0) == 0 && tls.get(0) == 0x11223344, "new worker values are independent and zero");
    tls.set_for(worker, 0, 0x55667788);
    require(tls.get(0) == 0x11223344 && tls.get_for(worker, 0) == 0x55667788, "worker set preserves parent values");
    require(tls.allocate_for(worker) == 1, "workers share the process allocation bitmap");
    tls.set(1, 3); tls.set_for(worker, 1, 4);
    tls.register_thread(next);
    require(tls.get_for(next, 0) == 0 && tls.get_for(next, 1) == 0, "later thread sees allocated slots zeroed");
    tls.free(0);
    require(memory.load<uint32_t>(worker) == 0 && memory.load<uint32_t>(base) == 0,
            "free clears slot in every registered thread");
    require(tls.allocate_for(next) == 0 && tls.get_for(worker, 0) == 0, "reused slot has no stale values");
    rejects([&] { tls.register_thread(worker); }, "tls-bank");
    rejects([&] { tls.register_thread(worker + 4); }, "tls-bank");
    rejects([&] { tls.get_for(0x73402000, 0); }, "tls-bank");
    memory.add_read_only_word(worker + 4, [] { return 4u; });
    const auto before = bytes(memory, base, 8);
    rejects([&] { tls.free(1); }, "memory-readonly");
    require(bytes(memory, base, 8) == before && tls.get(1) == 3, "free preflights all banks before any mutation");
}

int main() {
    struct Test { const char* name; void (*run)(); };
    int failures = 0;
    for (auto test : {Test{"allocation and coherence", allocation_and_coherence},
                      Test{"initialization and exact bounds", initialization_and_exact_bounds},
                      Test{"invalid indices and capacity", invalid_indices_and_capacity},
                      Test{"metadata rejection", metadata_rejection},
                      Test{"source and destination failures", source_and_destination_failures},
                      Test{"guard failures preserve ownership", guard_failures_preserve_ownership},
                      Test{"shared allocation, independent banks", independent_banks_share_allocation}}) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    if (failures) return 1;
    std::cout << "Thread local storage checks passed (7 groups)\n";
    return 0;
}
