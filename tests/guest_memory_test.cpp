#include "guest_memory.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename F> static void require_stop(F operation, const char* category, const char* message) {
    try { operation(); }
    catch (const sfr::RuntimeStop& e) {
        require(e.category == category, message);
        return;
    }
    throw std::runtime_error(message);
}

static void computed_reads() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 32);
    for (uint32_t i = 0; i < 32; ++i) memory.store<uint8_t>(0x10000 + i, uint8_t(i));
    std::array<uint8_t, 32> original{};
    std::copy_n(memory.base() + 0x10000, original.size(), original.begin());
    unsigned calls = 0;
    uint32_t value = 0x12345678;
    memory.add_read_only_word(0x10004, [&] { ++calls; return value++; });
    require(calls == 0, "registration does not call provider");
    require(memory.load<uint32_t>(0x10004) == 0x12345678 && calls == 1, "computed full word samples once");
    require(memory.load<uint32_t>(0x10004) == 0x12345679 && calls == 2, "computed reads change per load");
    value = 0x12345678;
    require(memory.load<uint8_t>(0x10005) == 0x34 && calls == 3, "computed byte is big endian");
    require(memory.load<uint16_t>(0x10005) == 0x3456 && calls == 4, "computed unaligned half word");
    require(memory.load<uint32_t>(0x10002) == 0x02031234 && calls == 5, "ordinary prefix merges with computed bytes");
    value = 0x12345678;
    require(memory.load<uint32_t>(0x10006) == 0x56780809 && calls == 6, "computed suffix merges with ordinary bytes");
    value = 0x12345678;
    require(memory.load<uint64_t>(0x10002) == 0x0203123456780809ull && calls == 7,
            "wide unaligned load samples entire provider once");
    unsigned second_calls = 0;
    memory.add_read_only_word(0x10008, [&] { ++second_calls; return 0xABCDEF01u; });
    value = 0x12345678;
    require(memory.load<uint64_t>(0x10004) == 0x12345678ABCDEF01ull && calls == 8 && second_calls == 1,
            "wide load samples adjacent providers once each");
    value = 0x12345678;
    require(memory.load<uint64_t>(0x10003) == 0x0312345678ABCDEFull && calls == 9 && second_calls == 2,
            "unaligned wide load partially overlaps two providers");
    require(memory.load<uint8_t>(0x10000) == 0 && memory.load<uint16_t>(0x10000) == 0x0001 &&
            memory.load<uint32_t>(0x10000) == 0x00010203 &&
            memory.load<uint64_t>(0x10010) == 0x1011121314151617ull && calls == 9 && second_calls == 2,
            "ordinary scalar paths do not query providers");
    require(std::equal(original.begin(), original.end(), memory.base() + 0x10000),
            "computed reads never change backing bytes");
}

static void computed_writes() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 32);
    for (uint32_t i = 0; i < 32; ++i) memory.store<uint8_t>(0x10000 + i, uint8_t(i));
    std::array<uint8_t, 32> original{};
    std::copy_n(memory.base() + 0x10000, original.size(), original.begin());
    unsigned calls = 0;
    memory.add_read_only_word(0x10008, [&] { ++calls; return 1u; });
    require_stop([&] { memory.store<uint8_t>(0x10009, 0); }, "memory-readonly", "computed byte store rejected");
    require_stop([&] { memory.store<uint16_t>(0x10007, 0); }, "memory-readonly", "prefix half store rejected");
    require_stop([&] { memory.store<uint32_t>(0x10008, 0); }, "memory-readonly", "full word store rejected");
    require_stop([&] { memory.store<uint32_t>(0x1000b, 0); }, "memory-readonly", "suffix store rejected");
    require_stop([&] { memory.store<uint64_t>(0x10004, 0); }, "memory-readonly", "wide store rejected atomically");
    require_stop([&] { memory.check_write(0x10000, 16); }, "memory-readonly", "whole object write check rejected");
    require(calls == 0, "write checks never call provider");
    require(std::equal(original.begin(), original.end(), memory.base() + 0x10000), "failed stores leave all bytes unchanged");
    memory.check_write(0x10000, 8);
    memory.check_write(0x1000c, 20);
    memory.store<uint32_t>(0x10004, 0x12345678);
    memory.store<uint64_t>(0x1000c, 0xABCDEF0123456789ull);
    require(memory.load<uint32_t>(0x10004) == 0x12345678 &&
            memory.load<uint64_t>(0x1000c) == 0xABCDEF0123456789ull, "stores abutting providers remain writable");
}

static void provider_registration() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 128);
    memory.map(0x20000, 6);
    memory.reserve(0x30000, 4);
    memory.add_import_variable(0x10004, "Guarded");
    auto value = [] { return 1u; };
    require_stop([&] { memory.add_read_only_word(0x10001, value); }, "memory-provider", "unaligned provider rejected");
    require_stop([&] { memory.add_read_only_word(0x20004, value); }, "memory-provider", "provider past logical mapping rejected");
    require_stop([&] { memory.add_read_only_word(0x30000, value); }, "memory-provider", "uncommitted provider rejected");
    require_stop([&] { memory.add_read_only_word(0, value); }, "memory-provider", "unmapped provider rejected");
    require_stop([&] { memory.add_read_only_word(0xfffffffc, value); }, "memory-provider", "unmapped high provider rejected");
    require_stop([&] { memory.add_read_only_word(0xffffffff, value); }, "memory-provider", "overflowing provider rejected");
    require_stop([&] { memory.add_read_only_word(0x10004, value); }, "memory-provider", "guarded provider rejected");
    require_stop([&] { memory.add_read_only_word(0x10000, {}); }, "memory-provider", "empty provider rejected");
    memory.add_read_only_word(0x10000, value);
    require_stop([&] { memory.add_read_only_word(0x10000, value); }, "memory-provider", "overlapping provider rejected");
    for (uint32_t i = 0; i < 15; ++i) memory.add_read_only_word(0x10008 + i * 4, value);
    require(memory.load<uint32_t>(0x10040) == 1, "sixteenth provider accepted");
    require_stop([&] { memory.add_read_only_word(0x10044, value); }, "memory-provider", "seventeenth provider rejected");
    memory.store<uint32_t>(0x10044, 0x76543210);
    require(memory.load<uint32_t>(0x10044) == 0x76543210, "failed registration leaves word ordinary");
}

static void provider_failures() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 12);
    unsigned calls = 0;
    memory.add_read_only_word(0x10008, [&] { ++calls; return 1u; });
    require_stop([&] { memory.load<uint64_t>(0x10008); }, "memory-access", "bounds checked before provider");
    memory.add_import_variable(0x10004, "PrefixGuard");
    require_stop([&] { memory.load<uint64_t>(0x10004); }, "import-variable", "guard checked before provider");
    require_stop([&] { memory.store<uint64_t>(0x10004, 0); }, "import-variable", "store preserves import guard precedence");
    require_stop([&] { memory.store<uint64_t>(0x10008, 0); }, "memory-access", "store preserves bounds precedence");
    require_stop([&] { memory.check_write(0x10008, 0); }, "memory-access", "empty write range rejected");
    require_stop([&] { memory.check_write(0xffffffff, 4); }, "memory-access", "overflow write range rejected");
    require(calls == 0, "failed accesses never call providers");
    memory.add_import_variable(0x10008, "LaterGuard");
    require_stop([&] { memory.load<uint32_t>(0x10008); }, "import-variable", "later guard still blocks computed read");
    require(calls == 0, "later guard does not query provider");
    memory.map(0xfffff000, 0x1000);
    memory.add_read_only_word(0xfffffffc, [&] { ++calls; return 0x12345678u; });
    require(memory.load<uint32_t>(0xfffffffc) == 0x12345678 && calls == 1, "provider at address space end works");
    require_stop([&] { memory.load<uint64_t>(0xfffffffc); }, "memory-access", "overflow read precedes provider");
    require(calls == 1, "overflow read does not call provider");
    memory.map(0x20000, 4);
    memory.store<uint32_t>(0x20000, 0x12345678);
    unsigned failed_calls = 0;
    memory.add_read_only_word(0x20000, [&]() -> uint32_t { ++failed_calls; throw std::logic_error("clock failed"); });
    bool propagated = false;
    try { memory.load<uint16_t>(0x20001); }
    catch (const std::logic_error& e) { propagated = std::string(e.what()) == "clock failed"; }
    require(propagated && failed_calls == 1, "provider exceptions propagate without retry");
    require(memory.base()[0x20000] == 0x12 && memory.base()[0x20003] == 0x78, "provider exception preserves backing");
    require_stop([&] { memory.store<uint32_t>(0x20000, 0); }, "memory-readonly", "throwing provider still rejects stores");
    require(failed_calls == 1, "store never invokes throwing provider");
}

static void reservation_increment() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 8);
    memory.store<uint32_t>(0x10000, 0x12345678);
    const auto value = memory.load_reserved_word(0x10000);
    require(value == 0x12345678, "reserved load returns big-endian word");
    require(memory.has_reservation(), "reserved load creates reservation");
    require(memory.store_conditional_word(0x10000, value + 1), "conditional increment succeeds");
    require(memory.load<uint32_t>(0x10000) == 0x12345679 &&
            memory.base()[0x10000] == 0x12 && memory.base()[0x10003] == 0x79,
            "conditional increment stores big-endian word");
    require(!memory.has_reservation(), "successful conditional store consumes reservation");
    require(!memory.store_conditional_word(0x10000, 0), "consumed reservation cannot store again");
    require(memory.load<uint32_t>(0x10000) == 0x12345679, "failed conditional store preserves bytes");
}

static void reservation_replacement() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 8);
    memory.store<uint32_t>(0x10000, 0x80000000u);
    memory.store<uint32_t>(0x10004, 0xffffffffu);
    require(memory.load_reserved_word(0x10000) == 0x80000000u, "reserved load preserves high bit");
    require(memory.load_reserved_word(0x10004) == 0xffffffffu, "replacement preserves full uint32 value");
    require_stop([&] { memory.store_conditional_word(0x10000, 1); }, "reservation-address",
                 "store to replaced reservation address stops");
    require(memory.has_reservation() && memory.load<uint32_t>(0x10000) == 0x80000000u &&
            memory.load<uint32_t>(0x10004) == 0xffffffffu, "address mismatch preserves reservation and memory");
    require(memory.store_conditional_word(0x10004, 0xfedcba98u), "replacement reservation remains usable");
    require(memory.load<uint32_t>(0x10004) == 0xfedcba98u && memory.base()[0x10004] == 0xfe &&
            memory.base()[0x10007] == 0x98, "conditional store preserves full uint32 big-endian value");
}

static void reservation_validation() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 16);
    memory.map(0x20000, 6);
    memory.reserve(0x30000, 4);
    memory.store<uint32_t>(0x10000, 0x12345678);
    memory.add_import_variable(0x10008, "ReservationGuard");
    unsigned calls = 0;
    memory.add_read_only_word(0x1000c, [&] { ++calls; return 0xdeadbeefu; });
    struct Invalid { uint64_t address; const char* category; };
    const Invalid invalid[] = {
        {0, "memory-access"}, {0x20004, "memory-access"}, {0x30000, "memory-access"},
        {0xfffffffc, "memory-access"}, {0x100000000ull, "memory-access"},
        {0xfffffffffffffffcull, "memory-access"},
        {0x10001, "memory-alignment"}, {0x10002, "memory-alignment"},
        {0x10003, "memory-alignment"}, {0xffffffff, "memory-alignment"},
        {0x10008, "import-variable"}, {0x1000c, "memory-readonly"},
    };
    for (bool active : {false, true}) {
        if (active) memory.load_reserved_word(0x10000);
        for (const auto& entry : invalid) {
            require_stop([&] { memory.load_reserved_word(entry.address); }, entry.category,
                         "invalid reserved load rejected before replacement");
            require(memory.has_reservation() == active, "invalid reserved load preserves reservation state");
            require_stop([&] { memory.store_conditional_word(entry.address, 0); }, entry.category,
                         "invalid conditional store rejected even without reservation");
            require(memory.has_reservation() == active, "invalid conditional store preserves reservation state");
            require(memory.load<uint32_t>(0x10000) == 0x12345678 && calls == 0,
                    "invalid reservation operations preserve bytes and never sample read-only providers");
        }
    }
    require(memory.store_conditional_word(0x10000, 0x87654321), "reservation survives every invalid operation");
    require(!memory.store_conditional_word(0x10004, 1) && memory.load<uint32_t>(0x10004) == 0,
            "valid conditional store without reservation returns false without writing");
}

static void reservation_interference() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 24);
    memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
    memory.store<uint64_t>(0x10008, 0x1020304050607080ull);
    std::array<uint8_t, 24> original{};
    std::copy_n(memory.base() + 0x10000, original.size(), original.begin());
    unsigned calls = 0;
    memory.add_read_only_word(0x10010, [&] { ++calls; return 7u; });
    memory.load_reserved_word(0x10000);
    require_stop([&] { memory.store<uint8_t>(0x10000, 0); }, "reservation-interference", "byte interference stops");
    require_stop([&] { memory.store<uint16_t>(0x10003, 0); }, "reservation-interference", "partial interference stops");
    require_stop([&] { memory.store<uint32_t>(0x10008, 0); }, "reservation-interference", "disjoint interference stops");
    require_stop([&] { memory.store<uint64_t>(0x10004, 0); }, "reservation-interference", "wide interference stops");
    require_stop([&] { memory.check_write(0x10014, 4); }, "reservation-interference", "write preflight stops");
    require_stop([&] { memory.check_write(0x10010, 4); }, "memory-readonly", "read-only validation precedes interference");
    require_stop([&] { memory.check_write(0, 4); }, "memory-access", "range validation precedes interference");
    require(memory.has_reservation() && calls == 0 &&
            std::equal(original.begin(), original.end(), memory.base() + 0x10000),
            "all ordinary write attempts preserve bytes and reservation without sampling providers");
    require(memory.store_conditional_word(0x10000, 0x12345679), "original conditional store survives interference stops");
    memory.store<uint32_t>(0x10008, 0xfedcba98);
    require(memory.load<uint32_t>(0x10008) == 0xfedcba98, "ordinary writes resume after reservation consumed");
}

static void reservation_changed_backing() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 4);
    memory.store<uint32_t>(0x10000, 0x12345678);
    memory.load_reserved_word(0x10000);
    // Explicit test-only bypass; raw host writes are outside the serialized guest-execution profile.
    memory.base()[0x10003] = 0x79;
    require(!memory.store_conditional_word(0x10000, 0xffffffffu), "changed backing fails conditional store");
    require(!memory.has_reservation() && memory.load<uint32_t>(0x10000) == 0x12345679,
            "changed backing consumes reservation without overwriting external value");
    require(!memory.store_conditional_word(0x10000, 0), "changed backing cannot retry consumed reservation");
}

static void reservation_independent_instances_and_top_address() {
    sfr::GuestMemory first;
    sfr::GuestMemory second;
    first.map(0xfffff000, 0x1000);
    second.map(0xfffff000, 0x1000);
    first.store<uint32_t>(0xfffffffc, 0xffffffffu);
    second.store<uint32_t>(0xfffffffc, 7);
    require(first.load_reserved_word(0xfffffffc) == 0xffffffffu, "last aligned guest word can be reserved");
    require(!second.has_reservation() && !second.store_conditional_word(0xfffffffc, 9),
            "different memory instance does not inherit reservation");
    require(first.has_reservation(), "other instance's failed store leaves reservation");
    second.store<uint32_t>(0xfffffffc, 8);
    require(first.store_conditional_word(0xfffffffc, 0) && first.load<uint32_t>(0xfffffffc) == 0,
            "top-address conditional store succeeds");
    // Like a console core, a host thread holds one reservation: reserving in
    // another instance replaces it.
    require(second.load_reserved_word(0xfffffffc) == 8, "other instance reserves same address");
    require(!first.store_conditional_word(0xfffffffc, 5) && first.load<uint32_t>(0xfffffffc) == 0,
            "replaced reservation fails without storing");
    require(second.store_conditional_word(0xfffffffc, 9) && second.load<uint32_t>(0xfffffffc) == 9,
            "memory instances keep conditional stores independent");
}

static void reservations_belong_to_threads() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x1000);
    memory.store<uint32_t>(0x10000, 1);
    require(memory.load_reserved_word(0x10000) == 1, "main thread reserves");
    bool other_saw = true, other_stored = false;
    std::thread([&] {
        other_saw = memory.has_reservation();
        other_stored = memory.store_conditional_word(0x10000, 3);
        memory.store<uint32_t>(0x10000, 2);  // ordinary store: this thread holds no reservation
    }).join();
    require(!other_saw && !other_stored, "another thread neither sees nor uses the reservation");
    require(memory.has_reservation(), "another thread's store leaves the reservation in place");
    require(!memory.store_conditional_word(0x10000, 4) && memory.load<uint32_t>(0x10000) == 2,
            "a changed word fails the conditional store");

    memory.store<uint32_t>(0x10000, 0);
    memory.store<uint64_t>(0x10008, 0);
    constexpr int threads = 4, rounds = 20000;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
        workers.emplace_back([&] {
            for (int i = 0; i < rounds; ++i) {
                uint32_t word;
                do word = memory.load_reserved_word(0x10000);
                while (!memory.store_conditional_word(0x10000, word + 1));
                uint64_t doubleword;
                do doubleword = memory.load_reserved_doubleword(0x10008);
                while (!memory.store_conditional_doubleword(0x10008, doubleword + 0x100000001ull));
            }
        });
    for (auto& worker : workers) worker.join();
    require(memory.load<uint32_t>(0x10000) == threads * rounds, "concurrent word increments are atomic");
    require(memory.load<uint64_t>(0x10008) == uint64_t(threads * rounds) * 0x100000001ull,
            "concurrent doubleword increments are atomic");
}

static void concurrent_reader_checks_while_layout_changes() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x1000);
    memory.map(0x20000, 0x10000);
    memory.add_import_variable(0x10ff0, "variable");  // makes 0x10000 a checked page
    std::atomic<bool> done{false};
    std::atomic<uint64_t> checks{0};
    std::thread reader([&] {
        sfr::GuestMemory::concurrent_reader = true;
        while (!done) {
            memory.check(0x10000, 4);
            require(memory.readable(0x10004, 4), "checked page stays readable");
            memory.store<uint32_t>(0x10008, uint32_t(checks));
            ++checks;
        }
    });
    // The owner commits and decommits elsewhere while the reader checks.
    for (int i = 0; i < 2000 || checks < 1000; ++i) {
        memory.decommit(0x28000, 0x1000);
        memory.commit(0x28000, 0x1000);
    }
    done = true;
    reader.join();
    require(checks >= 1000, "reader ran beside layout changes");
}

static void loads_beside_special_words() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x1000);
    memory.store<uint32_t>(0x10100, 0x11223344);
    memory.store<uint32_t>(0x10008, 0x55667788);
    memory.add_import_variable(0x10100, "variable");
    memory.add_read_only_word(0x10200, [] { return 0xCAFEF00Du; });
    require(memory.load<uint32_t>(0x10008) == 0x55667788, "a word beside the special ones loads directly");
    require(memory.load<uint16_t>(0x100FE) == 0, "a load ending before a variable loads");
    require_stop([&] { memory.load<uint32_t>(0x100FE); }, "import-variable", "a load overlapping a variable stops");
    require(memory.load<uint32_t>(0x10200) == 0xCAFEF00Du, "a computed word still comes from its provider");
    require(memory.load<uint16_t>(0x10202) == 0xF00D, "part of a computed word too");
}

static void doubleword_reservation_increment_and_consumption() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 16);
    memory.store<uint64_t>(0x10000, 0xfedcba9876543210ull);
    const auto value = memory.load_reserved_doubleword(0x10000);
    require(value == 0xfedcba9876543210ull, "reserved doubleword load preserves all high bits");
    require(memory.has_reservation(), "reserved doubleword load creates reservation");
    require(memory.store_conditional_doubleword(0x10000, value + 1),
            "conditional doubleword increment succeeds");
    require(memory.load<uint64_t>(0x10000) == 0xfedcba9876543211ull &&
            memory.base()[0x10000] == 0xfe && memory.base()[0x10007] == 0x11,
            "conditional doubleword store writes the actual value in big-endian order");
    require(!memory.has_reservation(), "successful doubleword conditional store consumes reservation");
    require(!memory.store_conditional_doubleword(0x10000, 0),
            "consumed doubleword reservation cannot store again");
    require(memory.load<uint64_t>(0x10000) == 0xfedcba9876543211ull,
            "conditional doubleword store without reservation preserves bytes");

    sfr::GuestMemory top;
    top.map(0xfffff000, 0x1000);
    top.store<uint64_t>(0xfffffff8, 0xffffffffffffffffull);
    require(top.load_reserved_doubleword(0xfffffff8) == 0xffffffffffffffffull &&
            top.store_conditional_doubleword(0xfffffff8, 0),
            "last aligned guest doubleword can be reserved and conditionally stored");
}

static void doubleword_reservation_changed_backing() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 8);
    memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
    memory.load_reserved_doubleword(0x10000);
    // Explicit test-only bypass for the supported comparison-failure path.
    memory.base()[0x10000] = 0x92;
    require(!memory.store_conditional_doubleword(0x10000, 0xffffffffffffffffull),
            "changed high doubleword byte fails the full-width comparison");
    require(!memory.has_reservation() && memory.load<uint64_t>(0x10000) == 0x923456789abcdef0ull,
            "failed doubleword comparison consumes reservation without overwriting bytes");
}

static void doubleword_reservation_validation() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 32);
    memory.map(0x20000, 6);
    memory.reserve(0x30000, 8);
    memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
    memory.add_import_variable(0x10010, "DoublewordReservationGuard");
    unsigned calls = 0;
    memory.add_read_only_word(0x10018, [&] { ++calls; return 0xdeadbeefu; });
    struct Invalid { uint64_t address; const char* category; };
    const Invalid invalid[] = {
        {0, "memory-access"}, {0x20000, "memory-access"}, {0x30000, "memory-access"},
        {0xfffffff8, "memory-access"}, {0x100000000ull, "memory-access"},
        {0xfffffffffffffff8ull, "memory-access"},
        {0x10001, "memory-alignment"}, {0x10004, "memory-alignment"},
        {0xffffffff, "memory-alignment"},
        {0x10010, "import-variable"}, {0x10018, "memory-readonly"},
    };
    for (bool active : {false, true}) {
        if (active) memory.load_reserved_word(0x10000);
        for (const auto& entry : invalid) {
            require_stop([&] { memory.load_reserved_doubleword(entry.address); }, entry.category,
                         "invalid reserved doubleword load is rejected before replacement");
            require(memory.has_reservation() == active,
                    "invalid reserved doubleword load preserves reservation state");
            require_stop([&] { memory.store_conditional_doubleword(entry.address, 0); }, entry.category,
                         "invalid conditional doubleword store is rejected without a reservation");
            require(memory.has_reservation() == active,
                    "invalid conditional doubleword store preserves reservation state");
            require(memory.load<uint64_t>(0x10000) == 0x123456789abcdef0ull && calls == 0,
                    "invalid doubleword operations preserve bytes and never sample providers");
        }
        if (active)
            require(memory.store_conditional_word(0x10000, 0x12345679),
                    "word reservation survives invalid doubleword operations");
    }
    require(!memory.store_conditional_doubleword(0x10008, 1) &&
            memory.load<uint64_t>(0x10008) == 0,
            "valid conditional doubleword store without reservation returns false");
}

static void mixed_width_reservations_are_explicit() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 16);
    memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
    memory.load_reserved_word(0x10000);
    require_stop([&] { memory.store_conditional_doubleword(0x10000, 1); }, "reservation-width",
                 "doubleword conditional store rejects a word reservation at the same address");
    require(memory.has_reservation() && memory.load<uint64_t>(0x10000) == 0x123456789abcdef0ull,
            "mixed-width rejection preserves word reservation and bytes");
    require(memory.store_conditional_word(0x10000, 0x12345678),
            "word reservation remains usable after width rejection");

    memory.load_reserved_word(0x10008);
    require(memory.load_reserved_doubleword(0x10000) == 0x123456789abcdef0ull,
            "successful doubleword load replaces a previous word reservation");
    require_stop([&] { memory.store_conditional_word(0x10000, 1); }, "reservation-width",
                 "word conditional store rejects a doubleword reservation at the same address");
    require(memory.has_reservation() &&
            memory.store_conditional_doubleword(0x10000, 0xfedcba9876543210ull),
            "doubleword reservation survives width rejection");

    memory.load_reserved_doubleword(0x10000);
    require_stop([&] { memory.store_conditional_doubleword(0x10008, 1); }, "reservation-address",
                 "doubleword conditional store rejects a different aligned address");
    require(memory.has_reservation() &&
            memory.load<uint64_t>(0x10000) == 0xfedcba9876543210ull &&
            memory.load<uint64_t>(0x10008) == 0,
            "doubleword address mismatch preserves reservation and all bytes");
    require(memory.store_conditional_doubleword(0x10000, 0x0102030405060708ull),
            "doubleword reservation remains usable after address rejection");
}

static void doubleword_reservation_interference() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 24);
    memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
    memory.load_reserved_doubleword(0x10000);
    require_stop([&] { memory.store<uint32_t>(0x10010, 1); }, "reservation-interference",
                 "ordinary disjoint write stops during a doubleword reservation");
    require(memory.has_reservation() && memory.load<uint32_t>(0x10010) == 0,
            "ordinary write interference preserves doubleword reservation and memory");
    require(memory.store_conditional_doubleword(0x10000, 0x123456789abcdef1ull),
            "doubleword reservation survives interference stops");
}

static void memory_accounting_basics() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(8 * page);
    require(memory.backing_budget() == 8 * page, "configured backing budget reported");
    auto usage = memory.usage();
    require(usage.reserved_bytes == 0 && usage.committed_bytes == 0, "empty memory has no usage");

    memory.reserve(0x10000, page + 1);
    usage = memory.usage();
    require(usage.reserved_bytes == 2 * page && usage.committed_bytes == 0,
            "reservation rounds its tail without charging committed backing");
    memory.commit(0x10000, 1);
    usage = memory.usage();
    require(usage.reserved_bytes == 2 * page && usage.committed_bytes == page,
            "logical one-byte commit charges one host page");
    memory.commit(0x10000, page);
    memory.commit(0x11000, 1);
    usage = memory.usage();
    require(usage.committed_bytes == 2 * page, "recommit and adjacent merging count unique host pages");

    const sfr::GuestMemory defaults;
    require(defaults.backing_budget() == sfr::GuestMemory::default_backing_budget,
            "default backing budget reported");
}

static void memory_accounting_ranges_and_guards() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(8 * page);
    memory.reserve(0x10000, 3 * page);
    memory.commit(0x11000, page);
    auto usage = memory.usage(0x11000, page);
    require(usage.reserved_bytes == page && usage.committed_bytes == page, "usage clips extents to query range");
    usage = memory.usage(0x13000, page);
    require(usage.reserved_bytes == 0 && usage.committed_bytes == 0, "usage reports empty aligned slice");

    memory.map(0xfffff000, page);
    usage = memory.usage(0xfffff000, page);
    require(usage.reserved_bytes == page && usage.committed_bytes == page, "usage includes final guest page");

    memory.add_import_variable(0x11000, "AccountingGuard");
    memory.add_read_only_word(0x11004, [] { return 7u; });
    usage = memory.usage(0x11000, page);
    require(usage.reserved_bytes == page && usage.committed_bytes == page,
            "logical access guards do not change physical accounting");

    for (const auto& range : std::array<std::array<uint64_t, 2>, 6>{{
             {{0, 0}}, {{1, page}}, {{0, 1}}, {{0xffffffffull, page}},
             {{0x100000000ull, page}}, {{0xfffff000ull, 2 * page}},
         }}) {
        require_stop([&] { (void)memory.usage(range[0], range[1]); }, "memory-statistics",
                     "invalid memory statistics range rejected");
    }
}

static void memory_accounting_budget_failures_are_transactional() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(2 * page);
    memory.map(0x10000, page);
    memory.store<uint32_t>(0x10000, 0x12345678);
    memory.reserve(0x20000, 2 * page);
    memory.commit(0x20000, page);
    memory.store<uint32_t>(0x20000, 0xabcdef01);

    require_stop([&] { memory.commit(0x21000, page); }, "memory-budget", "over-budget commit rejected");
    auto usage = memory.usage();
    require(usage.reserved_bytes == 3 * page && usage.committed_bytes == 2 * page,
            "failed commit preserves reservation and committed accounting");
    require(memory.load<uint32_t>(0x10000) == 0x12345678 && memory.load<uint32_t>(0x20000) == 0xabcdef01,
            "failed commit preserves existing data");
    require_stop([&] { (void)memory.load<uint8_t>(0x21000); }, "memory-access",
                 "failed commit leaves target inaccessible");

    sfr::GuestMemory just_fit(3 * page);
    just_fit.map(0x30000, 2 * page + 1);
    require(just_fit.usage().committed_bytes == 3 * page, "commit exactly at budget succeeds");
}

static void memory_accounting_invalid_budgets() {
    constexpr uint64_t page = 0x1000;
    for (const uint64_t budget : {uint64_t(0), uint64_t(1), uint64_t(page + 1), uint64_t(sfr::GuestMemory::address_space_size + page)})
        require_stop([&] { sfr::GuestMemory memory(budget); }, "memory-budget", "invalid backing budget rejected");
}

static void write_combined_mapping() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr uint64_t page = 0x1000;
    // The renderer reads these pages on the CPU, so they are cached unless
    // the console's mapping is asked for (see set_host_write_combining).
    {
        sfr::GuestMemory cached(4 * page);
        cached.map_write_combined(0x10000, page + 1);
        MEMORY_BASIC_INFORMATION plain{};
        require(VirtualQuery(cached.base() + 0x10000, &plain, sizeof(plain)) == sizeof(plain) &&
                plain.State == MEM_COMMIT && plain.Protect == PAGE_READWRITE,
                "a write-combined request is cached by default");
        cached.store<uint64_t>(0x10003, 0x123456789abcdef0ull);
        require(cached.load<uint64_t>(0x10003) == 0x123456789abcdef0ull,
                "cached write-combined mapping still round-trips");
    }
    sfr::GuestMemory::set_host_write_combining(true);
    struct Restore { ~Restore() { sfr::GuestMemory::set_host_write_combining(false); } } restore;
    sfr::GuestMemory memory(4 * page);
    memory.map_write_combined(0x10000, page + 1);
    require(memory.load<uint64_t>(0x10000) == 0, "write-combined memory is zero initialized");
    memory.store<uint64_t>(0x10003, 0x123456789abcdef0ull);
    require(memory.load<uint64_t>(0x10003) == 0x123456789abcdef0ull,
            "unaligned write-combined scalar roundtrip");

    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(memory.base() + 0x10000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT &&
            info.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE),
            "write-combined mapping has native host protection");
    memory.map(0x20000, page);
    require(VirtualQuery(memory.base() + 0x20000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
            "ordinary neighboring mapping remains normal memory");

    memory.store<uint32_t>(0x10ffd, 0xabcdef01);
    require(memory.load<uint32_t>(0x10ffd) == 0xabcdef01,
            "write-combined scalar crosses its logical page boundary");
    require_stop([&] { (void)memory.load<uint8_t>(0x11001); }, "memory-access",
                 "write-combined logical tail remains exact");
    require_stop([&] { memory.commit(0x10000, 1); }, "memory-cache",
                 "ordinary commit cannot change write-combined backing");
    require_stop([&] { memory.commit(0x11000, 1); }, "memory-cache",
                 "ordinary commit cannot change a rounded write-combined tail page");
    require(memory.load<uint64_t>(0x10003) == 0x123456789abcdef0ull,
            "cache conflict preserves write-combined data");
#endif
}

static void write_combined_conflicts_and_guards() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(4 * page);
    memory.map(0x10000, page);
    memory.store<uint32_t>(0x10000, 0x12345678);
    require_stop([&] { memory.map_write_combined(0x10000, 1); }, "memory-map",
                 "write-combined map still rejects logical reservation overlap");
    require(memory.load<uint32_t>(0x10000) == 0x12345678,
            "failed write-combined reservation preserves ordinary data");

    memory.reserve(0x20000, page + 1);
    memory.commit(0x20000, 1);
    require_stop([&] { memory.map_write_combined(0x21000, 1); }, "memory-map",
                 "rounded reservation tail prevents a second mapping");

    sfr::GuestMemory guarded(2 * page);
    guarded.map_write_combined(0x30000, page);
    guarded.add_import_variable(0x30004, "WcGuard");
    guarded.add_read_only_word(0x30008, [] { return 7u; });
    require_stop([&] { guarded.store<uint32_t>(0x30004, 1); }, "import-variable",
                 "import guard still precedes write-combined store");
    require_stop([&] { guarded.store<uint32_t>(0x30008, 1); }, "memory-readonly",
                 "read-only guard still precedes write-combined store");

    sfr::GuestMemory budget(page);
    budget.map(0x40000, page);
    budget.store<uint32_t>(0x40000, 0xabcdef01);
    require_stop([&] { budget.map_write_combined(0x50000, page); }, "memory-budget",
                 "write-combined map honors backing budget");
    require(budget.usage().committed_bytes == page &&
            budget.load<uint32_t>(0x40000) == 0xabcdef01,
            "write-combined budget failure preserves committed metadata and data");
    require_stop([&] { (void)budget.load<uint8_t>(0x50000); }, "memory-access",
                 "write-combined budget failure leaves target uncommitted");
#endif
}

static void write_combined_rejects_reserved_words() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    sfr::GuestMemory memory;
    memory.map(0x10000, 8);
    memory.map_write_combined(0x20000, 8);
    memory.store<uint32_t>(0x10000, 0x12345678);
    memory.store<uint32_t>(0x20000, 0xabcdef01);
    memory.load_reserved_word(0x10000);
    require_stop([&] { memory.load_reserved_word(0x20000); }, "memory-cache",
                 "reserved load rejects write-combined memory");
    require(memory.has_reservation(), "rejected write-combined reserved load preserves live reservation");
    require_stop([&] { memory.store_conditional_word(0x20000, 0); }, "memory-cache",
                 "conditional store rejects write-combined memory");
    require(memory.has_reservation() && memory.load<uint32_t>(0x20000) == 0xabcdef01,
            "rejected write-combined conditional store preserves reservation and bytes");
    require(memory.store_conditional_word(0x10000, 0x87654321),
            "ordinary reservation survives write-combined atomic rejections");

    memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
    memory.load_reserved_doubleword(0x10000);
    require_stop([&] { memory.load_reserved_doubleword(0x20000); }, "memory-cache",
                 "reserved doubleword load rejects write-combined memory");
    require_stop([&] { memory.store_conditional_doubleword(0x20000, 0); }, "memory-cache",
                 "conditional doubleword store rejects write-combined memory");
    require(memory.has_reservation() && memory.load<uint64_t>(0x10000) == 0x123456789abcdef0ull &&
            memory.store_conditional_doubleword(0x10000, 0xfedcba9876543210ull),
            "doubleword reservation survives write-combined atomic rejections");
#endif
}

static void placeholder_arena_is_released() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    uint8_t* base = nullptr;
    {
        sfr::GuestMemory memory;
        base = memory.base();
        memory.map(0x10000, 1);
        memory.map_write_combined(0x20000, 1);
    }
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(base, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE &&
            static_cast<uint8_t*>(info.BaseAddress) <= base &&
            static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize >=
                base + sfr::GuestMemory::address_space_size,
            "destructor releases the entire guest arena");
    require(VirtualQuery(base + 0x10000, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE,
            "destructor releases ordinary replacement allocation");
    require(VirtualQuery(base + 0x20000, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE,
            "destructor releases write-combined replacement allocation");
#endif
}

static void exact_middle_placeholder_can_be_mapped() {
#ifdef _WIN32
    // This checks how a write-combined page is handled, so ask for the
    // console's mapping (off by default, see set_host_write_combining).
    sfr::GuestMemory::set_host_write_combining(true);
    struct Restore { ~Restore() { sfr::GuestMemory::set_host_write_combining(false); } } restore;
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(3 * page);
    memory.map(0x10000, page);
    memory.map(0x12000, page);
    memory.map(0x11000, page);
    memory.store<uint64_t>(0x10ffc, 0x123456789abcdef0ull);
    memory.store<uint64_t>(0x11ffc, 0xfedcba9876543210ull);
    require(memory.load<uint64_t>(0x10ffc) == 0x123456789abcdef0ull &&
            memory.load<uint64_t>(0x11ffc) == 0xfedcba9876543210ull,
            "mapping an exact middle placeholder preserves adjacent partitions");
#if defined(_M_X64) || defined(__x86_64__)
    sfr::GuestMemory mixed(3 * page);
    mixed.map(0x20000, page);
    mixed.map(0x22000, page);
    mixed.store<uint32_t>(0x20000, 0x12345678);
    mixed.store<uint32_t>(0x22000, 0xabcdef01);
    mixed.map_write_combined(0x21000, page);
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(mixed.base() + 0x21000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE),
            "write-combined mapping replaces an exact middle placeholder");
    require(mixed.load<uint32_t>(0x20000) == 0x12345678 &&
            VirtualQuery(mixed.base() + 0x20000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
            "write-combined middle insertion preserves the left normal neighbor");
    require(mixed.load<uint32_t>(0x22000) == 0xabcdef01 &&
            VirtualQuery(mixed.base() + 0x22000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
            "write-combined middle insertion preserves the right normal neighbor");
#endif
#endif
}

static void cache_line_zero_all_offsets_and_address_edges() {
    sfr::GuestMemory memory;
    memory.map(0x210000, 0x200);
    for (uint32_t offset = 0; offset < 128; ++offset) {
        std::fill_n(memory.base() + 0x210000, 0x200, uint8_t{0xa5});
        memory.zero_cache_line(0x210080 + offset);
        require(std::all_of(memory.base() + 0x210080, memory.base() + 0x210100,
                           [](uint8_t byte) { return byte == 0; }) &&
                memory.base()[0x21007f] == 0xa5 && memory.base()[0x210100] == 0xa5,
                "cache-line zero clears exactly 128 aligned bytes for every effective offset");
    }
    memory.map(0, 128);
    std::fill_n(memory.base(), 128, uint8_t{0xcc});
    memory.zero_cache_line(0);
    require(std::all_of(memory.base(), memory.base() + 128, [](uint8_t byte) { return byte == 0; }),
            "cache-line zero handles first guest address");
    memory.map(0xfffff000u, 0x1000);
    std::fill_n(memory.base() + 0xffffff7full, 129, uint8_t{0xcc});
    memory.zero_cache_line(0xffffffffu);
    require(memory.base()[0xffffff7full] == 0xcc &&
            std::all_of(memory.base() + 0xffffff80ull, memory.base() + 0x100000000ull,
                        [](uint8_t byte) { return byte == 0; }),
            "cache-line zero handles last guest line without address overflow");
}

static void cache_line_zero_preflight_preserves_bytes_and_guards() {
    sfr::GuestMemory tail;
    tail.map(0x220000, 127);
    std::fill_n(tail.base() + 0x220000, 127, uint8_t{0x6a});
    require_stop([&] { tail.zero_cache_line(0x22007f); }, "memory-access", "short logical line rejects zero");
    require(std::all_of(tail.base() + 0x220000, tail.base() + 0x22007f,
                       [](uint8_t byte) { return byte == 0x6a; }), "short line rejection preserves every valid byte");
    for (bool provider : {false, true}) {
        sfr::GuestMemory guarded;
        guarded.map(0x230000, 128);
        std::fill_n(guarded.base() + 0x230000, 128, uint8_t{0x7b});
        unsigned calls = 0;
        if (provider) guarded.add_read_only_word(0x23007c, [&] { ++calls; return 0u; });
        else guarded.add_import_variable(0x23007c, "CacheLineGuard");
        require_stop([&] { guarded.zero_cache_line(0x230031); }, provider ? "memory-readonly" : "import-variable",
                     "guard in last word blocks entire cache-line zero");
        require(calls == 0 && std::all_of(guarded.base() + 0x230000, guarded.base() + 0x230080,
                           [](uint8_t byte) { return byte == 0x7b; }),
                "cache-line guard rejects before callback or any write");
    }
    sfr::GuestMemory atomic;
    atomic.map(0x240000, 256);
    std::fill_n(atomic.base() + 0x240000, 256, uint8_t{0x8c});
    atomic.load_reserved_word(0x240080);
    require_stop([&] { atomic.zero_cache_line(0x24007f); }, "reservation-interference",
                 "live reservation blocks cache-line zero even outside the reserved word");
    require(std::all_of(atomic.base() + 0x240000, atomic.base() + 0x240100,
                       [](uint8_t byte) { return byte == 0x8c; }) && atomic.has_reservation() &&
            atomic.store_conditional_word(0x240080, 9), "rejected zero preserves bytes and usable atomic reservation");
}

static void cache_line_zero_preserves_write_combined_mapping() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // This checks how a write-combined page is handled, so ask for the
    // console's mapping (off by default, see set_host_write_combining).
    sfr::GuestMemory::set_host_write_combining(true);
    struct Restore { ~Restore() { sfr::GuestMemory::set_host_write_combining(false); } } restore;
    sfr::GuestMemory memory;
    memory.map_write_combined(0x250000, 0x1000);
    std::fill_n(memory.base() + 0x25007f, 130, uint8_t{0x9d});
    memory.zero_cache_line(0x2500ff);
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(memory.base() + 0x250080, &info, sizeof(info)) == sizeof(info) &&
            info.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE) &&
            memory.base()[0x25007f] == 0x9d && memory.base()[0x250100] == 0x9d &&
            std::all_of(memory.base() + 0x250080, memory.base() + 0x250100,
                        [](uint8_t byte) { return byte == 0; }),
            "cache-line zero preserves WC protection and both neighbors");
#endif
}

static void cache_block_zero_alignment_and_atomicity() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x100);
    constexpr uint32_t block = 0x10020;
    for (uint32_t offset = 0; offset < 32; ++offset) {
        std::fill_n(memory.base() + 0x10000, 0x100, uint8_t{0xa5});
        memory.zero_cache_block(block + offset);
        require(std::all_of(memory.base() + block, memory.base() + block + 32,
                            [](uint8_t byte) { return byte == 0; }),
                "every effective-address offset zeroes its aligned 32-byte block");
        require(memory.base()[block - 1] == 0xa5 && memory.base()[block + 32] == 0xa5,
                "cache-block zero preserves both neighboring bytes");
    }

    sfr::GuestMemory short_tail;
    short_tail.map(0x20000, 31);
    std::fill_n(short_tail.base() + 0x20000, 31, uint8_t{0x7c});
    require_stop([&] { short_tail.zero_cache_block(0x2001f); }, "memory-access",
                 "logical range ending before the block rejects cache-block zero");
    require(std::all_of(short_tail.base() + 0x20000, short_tail.base() + 0x2001f,
                        [](uint8_t byte) { return byte == 0x7c; }),
            "failed range preflight leaves the whole writable prefix unchanged");

    sfr::GuestMemory guarded;
    guarded.map(0x30000, 32);
    std::fill_n(guarded.base() + 0x30000, 32, uint8_t{0x6d});
    unsigned calls = 0;
    guarded.add_read_only_word(0x3001c, [&] { ++calls; return 1u; });
    require_stop([&] { guarded.zero_cache_block(0x30000); }, "memory-readonly",
                 "read-only word in final bytes rejects whole cache-block zero");
    require(calls == 0 && std::all_of(guarded.base() + 0x30000, guarded.base() + 0x30020,
                                      [](uint8_t byte) { return byte == 0x6d; }),
            "guard failure performs no prior write or provider read");

    sfr::GuestMemory imported;
    imported.map(0x31000, 32);
    std::fill_n(imported.base() + 0x31000, 32, uint8_t{0x3e});
    imported.add_import_variable(0x3101c, "CacheBlockGuard");
    require_stop([&] { imported.zero_cache_block(0x31000); }, "import-variable",
                 "import guard in final bytes rejects whole cache-block zero");
    require(std::all_of(imported.base() + 0x31000, imported.base() + 0x31020,
                        [](uint8_t byte) { return byte == 0x3e; }),
            "import guard failure performs no partial zero");
}

static void cache_block_zero_preserves_reservations_and_wraps_ea() {
    sfr::GuestMemory memory;
    memory.map(0x40000, 32);
    std::fill_n(memory.base() + 0x40000, 32, uint8_t{0x4b});
    memory.store<uint32_t>(0x40000, 0x12345678);
    memory.load_reserved_word(0x40000);
    require_stop([&] { memory.zero_cache_block(0x4001f); }, "reservation-interference",
                 "live reservation rejects cache-block zero");
    require(memory.has_reservation() && memory.load<uint32_t>(0x40000) == 0x12345678 &&
            std::all_of(memory.base() + 0x40004, memory.base() + 0x40020,
                        [](uint8_t byte) { return byte == 0x4b; }),
            "reservation failure preserves reservation state and block data");
    require(memory.store_conditional_word(0x40000, 0x87654321),
            "reservation remains usable after rejected cache-block zero");

    sfr::GuestMemory top;
    top.map(0xfffff000, 0x1000);
    std::fill_n(top.base() + 0xffffffe0ull, 32, uint8_t{0xcc});
    top.zero_cache_block(0xffffffffu);
    require(std::all_of(top.base() + 0xffffffe0ull, top.base() + 0x100000000ull,
                        [](uint8_t byte) { return byte == 0; }),
            "final effective address zeroes the last aligned block without overflow");
}

static void cache_block_zero_write_combined_mapping() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    sfr::GuestMemory memory;
    memory.map_write_combined(0x50000, 0x1000);
    std::fill_n(memory.base() + 0x50020, 32, uint8_t{0x9e});
    MEMORY_BASIC_INFORMATION before{}, after{};
    require(VirtualQuery(memory.base() + 0x50020, &before, sizeof(before)) == sizeof(before),
            "write-combined cache-block fixture is queryable");
    memory.zero_cache_block(0x5003f);
    require(VirtualQuery(memory.base() + 0x50020, &after, sizeof(after)) == sizeof(after) &&
            after.Protect == before.Protect &&
            std::all_of(memory.base() + 0x50020, memory.base() + 0x50040,
                        [](uint8_t byte) { return byte == 0; }),
            "cache-block zero preserves write-combined protection and clears bytes");
#endif
}

static void decommit_splits_repeats_and_recommits_private_backing() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(4 * page);
    memory.reserve(0x60000, 3 * page);
    memory.commit(0x60000, 3 * page);
    memory.store<uint32_t>(0x60000, 0x11111111);
    memory.store<uint32_t>(0x61000, 0x22222222);
    memory.store<uint32_t>(0x62000, 0x33333333);

    memory.decommit(0x61000, page);
    auto usage = memory.usage(0x60000, 3 * page);
    require(usage.reserved_bytes == 3 * page && usage.committed_bytes == 2 * page,
            "middle decommit preserves reservation and splits committed extents");
    require(memory.load<uint32_t>(0x60000) == 0x11111111 &&
            memory.load<uint32_t>(0x62000) == 0x33333333,
            "middle decommit preserves both committed neighbors");
    require_stop([&] { (void)memory.load<uint8_t>(0x61000); }, "memory-access",
                 "decommitted middle page is logically inaccessible");
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(memory.base() + 0x61000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_RESERVE,
            "Windows middle decommit removes native commitment without releasing reservation");
#endif

    memory.decommit(0x61000, page);
    usage = memory.usage(0x60000, 3 * page);
    require(usage.reserved_bytes == 3 * page && usage.committed_bytes == 2 * page,
            "repeated decommit of an already reserved page is valid and idempotent");
    memory.commit(0x61000, page);
    require(memory.load<uint32_t>(0x61000) == 0,
            "recommit of existing private backing returns discarded zero bytes");
    require(memory.load<uint32_t>(0x60000) == 0x11111111 &&
            memory.load<uint32_t>(0x62000) == 0x33333333,
            "recommit preserves live neighboring private pages");
#ifdef _WIN32
    require(VirtualQuery(memory.base() + 0x61000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT,
            "Windows recommit restores native commitment in the owned private allocation");
#endif

    sfr::GuestMemory reserved;
    reserved.reserve(0x70000, page);
    reserved.decommit(0x70000, page);
    require(reserved.usage(0x70000, page).reserved_bytes == page &&
            reserved.usage(0x70000, page).committed_bytes == 0,
            "decommit of untouched reserved-only backing is valid without host mutation");
}

static void decommit_reclaims_budget_without_releasing_ownership() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory(2 * page);
    memory.map(0x80000, 2 * page);
    memory.store<uint32_t>(0x80000, 0xaaaaaaaa);
    memory.decommit(0x80000, page);
    require(memory.usage().committed_bytes == page,
            "decommit returns discarded pages to the committed backing budget");
    memory.reserve(0x90000, page);
    memory.commit(0x90000, page);
    require(memory.load<uint32_t>(0x90000) == 0,
            "reclaimed budget can commit a distinct owned reservation");
    memory.decommit(0x90000, page);
    memory.commit(0x80000, page);
    require(memory.load<uint32_t>(0x80000) == 0,
            "original private allocation can recommit after its budget is restored");
    require_stop([&] { memory.commit(0x90000, page); }, "memory-budget",
                 "recommitting an owned private chunk still obeys the backing budget");
}

static void decommit_preflight_failures_are_atomic() {
    constexpr uint64_t page = 0x1000;
    for (const auto& range : std::array<std::array<uint64_t, 2>, 5>{{
             {{0x10000, 0}}, {{0x10001, page}}, {{0x10000, page - 1}},
             {{0xffffffffull, page}}, {{0x100000000ull, page}},
         }}) {
        sfr::GuestMemory memory;
        memory.map(0x10000, 2 * page);
        memory.store<uint32_t>(0x10000, 0x12345678);
        require_stop([&] { memory.decommit(range[0], range[1]); }, "memory-decommit",
                     "invalid decommit range is rejected explicitly");
        require(memory.load<uint32_t>(0x10000) == 0x12345678 &&
                memory.usage(0x10000, 2 * page).committed_bytes == 2 * page,
                "invalid decommit performs no partial host or metadata mutation");
    }

    sfr::GuestMemory adjacent;
    adjacent.map(0x20000, page);
    adjacent.map(0x21000, page);
    require_stop([&] { adjacent.decommit(0x20000, 2 * page); }, "memory-decommit",
                 "decommit cannot cross adjacent logical reservations");
    require(adjacent.usage(0x20000, 2 * page).committed_bytes == 2 * page,
            "cross-reservation rejection preserves both committed allocations");

    sfr::GuestMemory guarded;
    guarded.map(0x30000, page);
    guarded.store<uint32_t>(0x30000, 0xabcdef01);
    guarded.add_import_variable(0x30ffc, "DecommitGuard");
    require_stop([&] { guarded.decommit(0x30000, page); }, "import-variable",
                 "import variable anywhere in target blocks decommit");
    require(guarded.load<uint32_t>(0x30000) == 0xabcdef01,
            "import-guard rejection preserves earlier bytes");

    sfr::GuestMemory computed;
    computed.map(0x40000, page);
    computed.store<uint32_t>(0x40000, 0x76543210);
    unsigned calls = 0;
    computed.add_read_only_word(0x40ffc, [&] { ++calls; return 7u; });
    require_stop([&] { computed.decommit(0x40000, page); }, "memory-readonly",
                 "computed read-only word anywhere in target blocks decommit");
    require(calls == 0 && computed.load<uint32_t>(0x40000) == 0x76543210,
            "computed guard rejection samples no provider and preserves bytes");

    sfr::GuestMemory reserved;
    reserved.map(0x50000, page);
    reserved.store<uint32_t>(0x50000, 0x13572468);
    reserved.load_reserved_word(0x50000);
    require_stop([&] { reserved.decommit(0x50000, page); }, "reservation-interference",
                 "live atomic reservation blocks decommit before effects");
    require(reserved.has_reservation() && reserved.load<uint32_t>(0x50000) == 0x13572468 &&
            reserved.store_conditional_word(0x50000, 0x24681357),
            "atomic rejection preserves data and usable reservation state");

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    sfr::GuestMemory combined;
    combined.map_write_combined(0xa0000, page);
    combined.store<uint32_t>(0xa0000, 0x11223344);
    require_stop([&] { combined.decommit(0xa0000, page); }, "memory-cache",
                 "write-combined backing rejects unsupported decommit");
    require(combined.load<uint32_t>(0xa0000) == 0x11223344,
            "write-combined decommit rejection preserves bytes");
#endif
}

static void decommit_preserves_private_allocation_boundaries_and_placeholders() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory adjacent;
    adjacent.reserve(0xb0000, 2 * page);
    adjacent.commit(0xb0000, page);
    adjacent.commit(0xb1000, page);
    adjacent.store<uint32_t>(0xb0000, 0x11112222);
    adjacent.store<uint32_t>(0xb1000, 0x33334444);
    adjacent.decommit(0xb0000, 2 * page);
    require(adjacent.usage(0xb0000, 2 * page).committed_bytes == 0,
            "one decommit crosses distinct adjacent private backing allocations");
    adjacent.commit(0xb0000, 2 * page);
    require(adjacent.load<uint32_t>(0xb0000) == 0 && adjacent.load<uint32_t>(0xb1000) == 0,
            "recommit restores zero pages across preserved private allocation boundaries");

    sfr::GuestMemory mixed;
    mixed.reserve(0xc0000, 3 * page);
    mixed.commit(0xc0000, page);
    mixed.commit(0xc2000, page);
    mixed.store<uint32_t>(0xc0000, 0x55556666);
    mixed.store<uint32_t>(0xc2000, 0x77778888);
    mixed.decommit(0xc0000, 3 * page);
    mixed.decommit(0xc0000, 3 * page);
    require(mixed.usage(0xc0000, 3 * page).reserved_bytes == 3 * page &&
            mixed.usage(0xc0000, 3 * page).committed_bytes == 0,
            "decommit spanning private-placeholder-private backing preserves the whole reservation");
    mixed.commit(0xc0000, 3 * page);
    require(mixed.load<uint32_t>(0xc0000) == 0 && mixed.load<uint32_t>(0xc1000) == 0 &&
            mixed.load<uint32_t>(0xc2000) == 0,
            "mixed backing recommit restores existing chunks and replaces untouched placeholder with zeros");
}

static void destructor_releases_split_private_allocations_and_later_mappings() {
#ifdef _WIN32
    constexpr uint64_t page = 0x1000;
    uint8_t* base = nullptr;
    {
        sfr::GuestMemory memory;
        base = memory.base();
        memory.map(0xd0000, 3 * page);
        memory.store<uint32_t>(0xd0000, 0x11111111);
        memory.store<uint32_t>(0xd2000, 0x22222222);
        memory.decommit(0xd1000, page);
        memory.map(0xe0000, page);
        memory.store<uint32_t>(0xe0000, 0x33333333);
    }
    for (uint64_t address : {0xd0000ull, 0xd1000ull, 0xd2000ull, 0xe0000ull}) {
        MEMORY_BASIC_INFORMATION info{};
        require(VirtualQuery(base + address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE,
                "destructor releases every region of a split private allocation and later mappings");
    }
#endif
}

static void release_removes_whole_reservations_and_reuses_zeroed_addresses() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory;
    memory.map(0xf0000, page + 1);
    memory.map(0xf2000, page);
    memory.store<uint32_t>(0xf0000, 0x11112222);
    memory.store<uint8_t>(0xf1000, 0x33);
    memory.store<uint32_t>(0xf2000, 0x55556666);
    memory.release(0xf0000, page + 1);
    require(memory.available(0xf0000, page + 1), "whole release removes the exact logical reservation");
    const auto released = memory.usage(0xf0000, 2 * page);
    require(released.reserved_bytes == 0 && released.committed_bytes == 0,
            "whole release removes reservation and commitment accounting over rounded tail");
    require(memory.load<uint32_t>(0xf2000) == 0x55556666,
            "whole release preserves neighboring reservation bytes");
    require_stop([&] { (void)memory.load<uint8_t>(0xf0000); }, "memory-access",
                 "released guest address is inaccessible");
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(memory.base() + 0xf0000, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_RESERVE,
            "Windows release restores a placeholder inside the continuous guest envelope");
#endif
    memory.reserve(0xf0000, page + 1);
    memory.commit(0xf0000, page + 1);
    require(memory.load<uint32_t>(0xf0000) == 0 && memory.load<uint8_t>(0xf1000) == 0,
            "same-address reuse returns zeroed pages across the rounded logical tail");
}

static void release_handles_decommitted_reserved_and_multi_chunk_backing() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory partial;
    partial.map(0x110000, 3 * page);
    partial.decommit(0x111000, page);
    partial.release(0x110000, 3 * page);
    require(partial.available(0x110000, 3 * page),
            "whole release converts partially decommitted private backing to reusable space");

    sfr::GuestMemory reserved;
    reserved.reserve(0x120000, 1);
    reserved.release(0x120000, 1);
    require(reserved.available(0x120000, page),
            "whole release removes exact one-byte reserved-only ownership and its rounded span");

    sfr::GuestMemory fully_decommitted;
    fully_decommitted.map(0x121000, page);
    fully_decommitted.decommit(0x121000, page);
    fully_decommitted.release(0x121000, page);
    require(fully_decommitted.available(0x121000, page),
            "whole release restores a fully decommitted retained private allocation");

    sfr::GuestMemory chunks;
    chunks.reserve(0x130000, 3 * page);
    chunks.commit(0x130000, page);
    chunks.commit(0x131000, page);
    chunks.commit(0x132000, page);
    chunks.store<uint32_t>(0x130000, 1);
    chunks.store<uint32_t>(0x131000, 2);
    chunks.store<uint32_t>(0x132000, 3);
    chunks.release(0x130000, 3 * page);
    require(chunks.available(0x130000, 3 * page),
            "whole release restores every distinct private backing allocation");
#ifdef _WIN32
    for (uint64_t address : {0x130000ull, 0x131000ull, 0x132000ull}) {
        MEMORY_BASIC_INFORMATION info{};
        require(VirtualQuery(chunks.base() + address, &info, sizeof(info)) == sizeof(info) &&
                info.State == MEM_RESERVE,
                "each distinct released private allocation is restored as a placeholder");
    }
#endif
    chunks.map(0x130000, 3 * page);
    require(chunks.load<uint32_t>(0x130000) == 0 && chunks.load<uint32_t>(0x131000) == 0 &&
            chunks.load<uint32_t>(0x132000) == 0,
            "multiple released private chunks can be reused with fresh zero contents");

    sfr::GuestMemory merged;
    merged.map(0x1b0000, page);
    merged.map(0x1b1000, page);
    merged.store<uint32_t>(0x1b0000, 0xaaaaaaaa);
    merged.store<uint32_t>(0x1b1000, 0xbbbbbbbb);
    merged.release(0x1b0000, page);
    require(merged.load<uint32_t>(0x1b1000) == 0xbbbbbbbb,
            "release splits committed metadata merged across adjacent reservations");
    merged.release(0x1b1000, page);
    merged.map(0x1b0000, 2 * page);
    require(merged.load<uint32_t>(0x1b0000) == 0 && merged.load<uint32_t>(0x1b1000) == 0,
            "released placeholder fragments support larger reuse spanning old reservation boundaries");

    sfr::GuestMemory split;
    split.map(0x1c0000, page);
    split.map(0x1c1000, page);
    split.map(0x1c2000, page);
    split.store<uint32_t>(0x1c0000, 0x11111111);
    split.store<uint32_t>(0x1c1000, 0x22222222);
    split.store<uint32_t>(0x1c2000, 0x33333333);
    split.release(0x1c1000, page);
    require(split.load<uint32_t>(0x1c0000) == 0x11111111 &&
            split.load<uint32_t>(0x1c2000) == 0x33333333 &&
            split.usage(0x1c0000, 3 * page).committed_bytes == 2 * page,
            "middle release splits one merged committed extent and preserves both neighbors");

    sfr::GuestMemory mixed_release;
    mixed_release.reserve(0x1d0000, 3 * page);
    mixed_release.commit(0x1d0000, page);
    mixed_release.commit(0x1d2000, page);
    mixed_release.release(0x1d0000, 3 * page);
#ifdef _WIN32
    for (uint64_t address : {0x1d0000ull, 0x1d1000ull, 0x1d2000ull}) {
        MEMORY_BASIC_INFORMATION info{};
        require(VirtualQuery(mixed_release.base() + address, &info, sizeof(info)) == sizeof(info) &&
                info.State == MEM_RESERVE,
                "release preserves the native envelope across private and untouched placeholder pages");
    }
#endif
    mixed_release.map(0x1d0000, 3 * page);
    require(mixed_release.load<uint32_t>(0x1d0000) == 0 &&
            mixed_release.load<uint32_t>(0x1d1000) == 0 &&
            mixed_release.load<uint32_t>(0x1d2000) == 0,
            "private-placeholder-private release span supports fresh zeroed reuse");
}

static void release_rejections_are_atomic() {
    constexpr uint64_t page = 0x1000;
    sfr::GuestMemory memory;
    memory.map(0x140000, 2 * page);
    memory.store<uint32_t>(0x140000, 0xabcdef01);
    for (const auto& range : std::array<std::array<uint64_t, 2>, 7>{{
             {{0x140000, 0}}, {{0x140001, 2 * page}}, {{0x141000, page}},
             {{0x140000, page}}, {{0x140000, 3 * page}},
             {{0xffffffffull, 2}}, {{0x100000000ull, page}},
         }}) {
        require_stop([&] { memory.release(range[0], range[1]); }, "memory-release",
                     "non-exact, interior, malformed, or unknown release is rejected");
        require(memory.load<uint32_t>(0x140000) == 0xabcdef01 &&
                memory.usage(0x140000, 2 * page).committed_bytes == 2 * page,
                "invalid release preserves reservation metadata and bytes");
    }

    sfr::GuestMemory guarded;
    guarded.map(0x150000, page);
    guarded.store<uint32_t>(0x150000, 0x12345678);
    guarded.add_import_variable(0x150ffc, "ReleaseGuard");
    require_stop([&] { guarded.release(0x150000, page); }, "import-variable",
                 "import guard blocks whole release before effects");
    require(guarded.load<uint32_t>(0x150000) == 0x12345678,
            "import-guard release rejection preserves bytes");

    sfr::GuestMemory computed;
    computed.map(0x160000, page);
    computed.store<uint32_t>(0x160000, 0x87654321);
    unsigned calls = 0;
    computed.add_read_only_word(0x160ffc, [&] { ++calls; return 1u; });
    require_stop([&] { computed.release(0x160000, page); }, "memory-readonly",
                 "computed provider blocks whole release before effects");
    require(calls == 0 && computed.load<uint32_t>(0x160000) == 0x87654321,
            "provider release rejection samples nothing and preserves bytes");

    sfr::GuestMemory atomic;
    atomic.map(0x170000, page);
    atomic.store<uint32_t>(0x170000, 0x10203040);
    atomic.load_reserved_word(0x170000);
    require_stop([&] { atomic.release(0x170000, page); }, "reservation-interference",
                 "live atomic reservation blocks whole release");
    require(atomic.has_reservation() && atomic.load<uint32_t>(0x170000) == 0x10203040 &&
            atomic.store_conditional_word(0x170000, 0x50607080),
            "atomic release rejection preserves usable reservation and data");

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    sfr::GuestMemory combined;
    combined.map_write_combined(0x180000, page);
    combined.store<uint32_t>(0x180000, 0xdeadbeef);
    require_stop([&] { combined.release(0x180000, page); }, "memory-cache",
                 "write-combined reservation rejects unsupported whole release");
    require(combined.load<uint32_t>(0x180000) == 0xdeadbeef,
            "write-combined release rejection preserves bytes");
#endif

    sfr::GuestMemory repeated;
    repeated.reserve(0x190000, page);
    repeated.release(0x190000, page);
    require_stop([&] { repeated.release(0x190000, page); }, "memory-release",
                 "repeated release rejects absent logical ownership");
}

static void teardown_after_release_still_frees_guest_envelope() {
#ifdef _WIN32
    uint8_t* base = nullptr;
    {
        sfr::GuestMemory memory;
        base = memory.base();
        memory.map(0x1a0000, 0x2000);
        memory.release(0x1a0000, 0x2000);
    }
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(base + 0x1a0000, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE,
            "destructor releases the restored placeholder envelope after guest release");
#endif
}

int main() {
    try {
        sfr::GuestMemory memory;
        memory.map(0x10000, 0x1000);
        memory.store<uint32_t>(0x10003, 0x12345678);
        require(memory.base()[0x10003] == 0x12 && memory.base()[0x10006] == 0x78, "big-endian bytes");
        require(memory.load<uint32_t>(0x10003) == 0x12345678, "unaligned roundtrip");
        memory.store<uint64_t>(0x10010, 0xFEDCBA9876543210ull);
        require(memory.load<uint64_t>(0x10010) == 0xFEDCBA9876543210ull, "64-bit roundtrip");
        for (uint64_t address : {0ull, 0x10ffeull, 0xfffffffeull, 0x100000000ull}) {
            bool rejected = false;
            try { memory.load<uint32_t>(address); } catch (const sfr::RuntimeStop&) { rejected = true; }
            require(rejected, "out-of-range access must fail");
        }
        bool overlap = false;
        try { memory.map(0x10000, 0x1000); } catch (const sfr::RuntimeStop&) { overlap = true; }
        require(overlap, "overlapping map rejected");
        memory.add_import_variable(0x10020, "TestVariable");
        bool blocked = false;
        try { memory.load<uint32_t>(0x10020); }
        catch (const sfr::RuntimeStop& e) {
            blocked = e.category == "import-variable" && e.address == 0x10020 && e.detail == "TestVariable";
        }
        require(blocked, "unimplemented variable read must identify import");
        blocked = false;
        try { memory.store<uint16_t>(0x10022, 0); }
        catch (const sfr::RuntimeStop&) { blocked = true; }
        require(blocked, "partial write to unimplemented variable rejected");
        auto rejects = [](auto operation) {
            try { operation(); } catch (const sfr::RuntimeStop&) { return true; }
            return false;
        };
        memory.reserve(0x20000, 0x3001);
        require(rejects([&] { memory.load<uint32_t>(0x20000); }), "reservation must be inaccessible");
        memory.commit(0x21000, 0x1000);
        require(memory.load<uint32_t>(0x21000) == 0, "new commit is zero initialized");
        require(rejects([&] { memory.load<uint32_t>(0x20000); }), "uncommitted prefix inaccessible");
        memory.store<uint32_t>(0x21000, 0x12345678);
        memory.commit(0x20000, 0x2000);
        require(memory.load<uint32_t>(0x21000) == 0x12345678, "recommit preserves existing bytes");
        memory.store<uint32_t>(0x20ffe, 0xabcdef01);
        require(memory.load<uint32_t>(0x20ffe) == 0xabcdef01, "scalar crosses adjacent commits");
        memory.commit(0x23000, 1);
        require(rejects([&] { memory.load<uint16_t>(0x23000); }), "logical commit bounds exact");
        require(rejects([&] { memory.commit(0x23000, 2); }), "commit outside logical reservation rejected");
        require(rejects([&] { memory.reserve(0x23000, 0x1000); }), "rounded reservation overlap rejected");
        require(rejects([&] { memory.commit(0x30000, 0x1000); }), "commit without reserve rejected");
        require(rejects([&] { memory.reserve(0xfffff000, 0x1001); }), "reservation overflow rejected");
        require(rejects([&] { memory.commit(0xfffff000, 0x1001); }), "commit overflow rejected");
        memory.map(0x40000, 0x1000);
        memory.map(0x41000, 0x1000);
        memory.store<uint64_t>(0x40ffc, 0x123456789abcdef0ull);
        require(memory.load<uint64_t>(0x40ffc) == 0x123456789abcdef0ull, "scalar crosses adjacent mappings");
        memory.map(0x50000, 1);
        require(rejects([&] { memory.reserve(0x50000, 0x1000); }), "map occupies rounded host page");
        unsigned failures = 0;
        for (auto test : {computed_reads, computed_writes, provider_registration, provider_failures,
                          reservation_increment, reservation_replacement, reservation_validation,
                          reservation_interference, reservation_changed_backing,
                          reservation_independent_instances_and_top_address, reservations_belong_to_threads,
                          concurrent_reader_checks_while_layout_changes,
                          loads_beside_special_words,
                          memory_accounting_basics,
                          doubleword_reservation_increment_and_consumption,
                          doubleword_reservation_changed_backing, doubleword_reservation_validation,
                          mixed_width_reservations_are_explicit, doubleword_reservation_interference,
                          memory_accounting_ranges_and_guards,
                          memory_accounting_budget_failures_are_transactional,
                          memory_accounting_invalid_budgets, write_combined_mapping,
                          write_combined_conflicts_and_guards,
                          write_combined_rejects_reserved_words, placeholder_arena_is_released,
                          exact_middle_placeholder_can_be_mapped,
                          cache_block_zero_alignment_and_atomicity,
                          cache_block_zero_preserves_reservations_and_wraps_ea,
                          cache_block_zero_write_combined_mapping,
                          decommit_splits_repeats_and_recommits_private_backing,
                          decommit_reclaims_budget_without_releasing_ownership,
                          decommit_preflight_failures_are_atomic,
                          decommit_preserves_private_allocation_boundaries_and_placeholders,
                          destructor_releases_split_private_allocations_and_later_mappings,
                          release_removes_whole_reservations_and_reuses_zeroed_addresses,
                          release_handles_decommitted_reserved_and_multi_chunk_backing,
                          release_rejections_are_atomic,
                          teardown_after_release_still_frees_guest_envelope,
                          cache_line_zero_all_offsets_and_address_edges,
                          cache_line_zero_preflight_preserves_bytes_and_guards,
                          cache_line_zero_preserves_write_combined_mapping}) {
            try { test(); }
            catch (const std::exception& e) { std::cerr << e.what() << '\n'; ++failures; }
        }
        require(failures == 0, "guest memory checks failed");
        std::cout << "Guest memory checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
