#include "guest_memory.h"
#include "vector_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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

static sfr::VectorBytes distinct_vector() {
    sfr::VectorBytes value{};
    for (uint8_t i = 0; i < value.size(); ++i) value[i] = uint8_t(0x10 + i * 7);
    return value;
}

static void load_reverses_guest_block_for_every_effective_address_offset() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x1000);
    for (uint32_t i = 0; i < 16; ++i) memory.store<uint8_t>(0x10120 + i, uint8_t(0xa0 + i));
    sfr::VectorBytes expected{};
    for (uint32_t i = 0; i < 16; ++i) expected[i] = uint8_t(0xaf - i);
    for (uint32_t offset = 0; offset < 16; ++offset)
        require(sfr::load_vector_memory(memory, 0x10120 + offset) == expected,
                "vector load aligns down and reverses all sixteen guest bytes");
}

static void store_roundtrip_preserves_neighbors_and_source() {
    sfr::GuestMemory memory;
    memory.map(0x20000, 0x1000);
    std::fill_n(memory.base() + 0x2010f, 18, uint8_t(0xcc));
    const auto source = distinct_vector();
    const auto original = source;
    sfr::store_vector_memory(memory, 0x2011f, source);
    require(source == original, "vector store preserves its source");
    require(memory.base()[0x2010f] == 0xcc && memory.base()[0x20120] == 0xcc,
            "vector store preserves neighboring bytes");
    require(sfr::load_vector_memory(memory, 0x2011f) == source,
            "distinct byte vector roundtrips through guest memory");
    for (uint32_t i = 0; i < 16; ++i)
        require(memory.base()[0x20110 + i] == source[15 - i],
                "vector store reverses all sixteen bytes into guest order");
}

static void store_aligns_down_for_every_effective_address_offset() {
    sfr::GuestMemory memory;
    memory.map(0x28000, 0x1000);
    const auto source = distinct_vector();
    for (uint32_t offset = 0; offset < 16; ++offset) {
        std::fill_n(memory.base() + 0x2810f, 18, uint8_t(0xcc));
        sfr::store_vector_memory(memory, 0x28110 + offset, source);
        require(memory.base()[0x2810f] == 0xcc && memory.base()[0x28120] == 0xcc,
                "offset vector store preserves neighboring bytes");
        for (uint32_t i = 0; i < 16; ++i)
            require(memory.base()[0x28110 + i] == source[15 - i],
                    "vector store aligns down for every effective address offset");
    }
}

static void top_address_aligns_to_last_complete_block() {
    sfr::GuestMemory memory;
    memory.map(0xfffff000, 0x1000);
    const auto value = distinct_vector();
    sfr::store_vector_memory(memory, 0xffffffffu, value);
    require(sfr::load_vector_memory(memory, 0xffffffffu) == value,
            "effective address ffffffff accesses block fffffff0 through ffffffff");
}

static void complete_range_is_checked_before_reading() {
    sfr::GuestMemory tail;
    tail.map(0x30000, 8);
    require_stop([&] { (void)sfr::load_vector_memory(tail, 0x30007); }, "memory-access",
                 "vector load rejects a missing second half");

    sfr::GuestMemory guarded;
    guarded.map(0x40000, 16);
    guarded.add_import_variable(0x4000c, "SecondHalfGuard");
    require_stop([&] { (void)sfr::load_vector_memory(guarded, 0x40000); }, "import-variable",
                 "vector load checks a second-half guard before reading");
}

static void complete_store_range_is_checked_before_writing() {
    const auto source = distinct_vector();

    sfr::GuestMemory tail;
    tail.map(0x48000, 8);
    std::fill_n(tail.base() + 0x48000, 8, uint8_t(0x3c));
    require_stop([&] { sfr::store_vector_memory(tail, 0x48007, source); }, "memory-access",
                 "vector store rejects a missing logical second half");
    require(std::all_of(tail.base() + 0x48000, tail.base() + 0x48008,
                        [](uint8_t byte) { return byte == 0x3c; }),
            "missing second half leaves the valid prefix unchanged");

    sfr::GuestMemory guarded;
    guarded.map(0x49000, 16);
    std::fill_n(guarded.base() + 0x49000, 16, uint8_t(0x4d));
    guarded.add_import_variable(0x4900c, "StoreSecondHalfGuard");
    require_stop([&] { sfr::store_vector_memory(guarded, 0x49009, source); }, "import-variable",
                 "vector store rejects a second-half guard");
    require(std::all_of(guarded.base() + 0x49000, guarded.base() + 0x49010,
                        [](uint8_t byte) { return byte == 0x4d; }),
            "second-half guard leaves the writable prefix unchanged");
}

static void complete_load_check_precedes_provider_sampling() {
    sfr::GuestMemory memory;
    memory.map(0x4a000, 16);
    unsigned calls = 0;
    memory.add_read_only_word(0x4a004, [&] { ++calls; return 0x11223344u; });
    memory.add_import_variable(0x4a00c, "LoadSecondHalfGuard");
    require_stop([&] { (void)sfr::load_vector_memory(memory, 0x4a007); }, "import-variable",
                 "complete vector load check detects second-half guard");
    require(calls == 0, "complete vector load check runs before first-half provider sampling");
}

static void computed_words_are_sampled_once_per_containing_word() {
    sfr::GuestMemory memory;
    memory.map(0x50000, 16);
    unsigned first_calls = 0;
    unsigned second_calls = 0;
    memory.add_read_only_word(0x50004, [&] { ++first_calls; return 0x44556677u; });
    memory.add_read_only_word(0x5000c, [&] { ++second_calls; return 0xccddeeffu; });
    const auto value = sfr::load_vector_memory(memory, 0x50009);
    require(first_calls == 1 && second_calls == 1,
            "vector load samples each computed word once through two wide reads");
    require(value[11] == 0x44 && value[8] == 0x77 && value[3] == 0xcc && value[0] == 0xff,
            "vector load places computed big-endian bytes in reversed internal order");
}

static void provider_exception_in_second_half_does_not_replace_destination() {
    sfr::GuestMemory memory;
    memory.map(0x60000, 16);
    unsigned first_calls = 0;
    unsigned failed_calls = 0;
    memory.add_read_only_word(0x60004, [&] { ++first_calls; return 0x11223344u; });
    memory.add_read_only_word(0x6000c, [&]() -> uint32_t {
        ++failed_calls;
        throw std::logic_error("second half failed");
    });
    auto destination = distinct_vector();
    const auto original = destination;
    bool propagated = false;
    try { destination = sfr::load_vector_memory(memory, 0x60000); }
    catch (const std::logic_error& e) { propagated = std::string(e.what()) == "second half failed"; }
    require(propagated && first_calls == 1 && failed_calls == 1,
            "second-half provider exception propagates without retries");
    require(destination == original, "failed vector load leaves caller destination unchanged");
}

static void failed_stores_do_not_partially_write() {
    const auto source = distinct_vector();

    sfr::GuestMemory read_only;
    read_only.map(0x70000, 32);
    std::fill_n(read_only.base() + 0x70000, 32, uint8_t(0x5a));
    std::array<uint8_t, 32> before{};
    std::copy_n(read_only.base() + 0x70000, before.size(), before.begin());
    unsigned calls = 0;
    read_only.add_read_only_word(0x7000c, [&] { ++calls; return 1u; });
    require_stop([&] { sfr::store_vector_memory(read_only, 0x70003, source); }, "memory-readonly",
                 "read-only last word rejects complete vector store");
    require(calls == 0 && std::equal(before.begin(), before.end(), read_only.base() + 0x70000),
            "read-only failure samples no provider and writes no prefix");

    sfr::GuestMemory reserved;
    reserved.map(0x80000, 32);
    std::fill_n(reserved.base() + 0x80000, 32, uint8_t(0x6b));
    std::copy_n(reserved.base() + 0x80000, before.size(), before.begin());
    reserved.load_reserved_word(0x80010);
    require_stop([&] { sfr::store_vector_memory(reserved, 0x80007, source); }, "reservation-interference",
                 "active reservation rejects vector store before mutation");
    require(std::equal(before.begin(), before.end(), reserved.base() + 0x80000) && source == distinct_vector(),
            "reservation failure preserves all guest bytes and source");
}

static void word_store_selects_by_aligned_effective_address() {
    sfr::GuestMemory memory;
    memory.map(0x90000, 0x1000);
    const auto source = distinct_vector();
    const auto original = source;

    for (uint32_t offset = 0; offset < 16; ++offset) {
        std::fill_n(memory.base() + 0x90100, 20, uint8_t(0xcc));
        sfr::store_vector_word(memory, 0x90100 + offset, source);

        const uint32_t address = (0x90100 + offset) & ~uint32_t(3);
        const uint32_t index = 12 - (address & 12);
        for (uint32_t i = 0; i < 20; ++i) {
            const uint32_t guest_address = 0x90100 + i;
            const bool written = guest_address >= address && guest_address < address + 4;
            const uint8_t expected = written ? source[index + 3 - (guest_address - address)] : uint8_t(0xcc);
            require(memory.base()[guest_address] == expected,
                    "word store writes exactly the selected four guest bytes");
        }
        require(source == original, "word store preserves its vector source");
    }
}

static void word_store_checks_only_its_aligned_word() {
    const auto source = distinct_vector();

    sfr::GuestMemory four_bytes;
    four_bytes.map(0xa0000, 4);
    sfr::store_vector_word(four_bytes, 0xa0003, source);
    for (uint32_t i = 0; i < 4; ++i)
        require(four_bytes.base()[0xa0000 + i] == source[15 - i],
                "word store does not require a mapped sixteen-byte block");

    sfr::GuestMemory top;
    top.map(0xfffff000, 0x1000);
    sfr::store_vector_word(top, 0xffffffffu, source);
    for (uint32_t i = 0; i < 4; ++i)
        require(top.base()[0xfffffffcull + i] == source[3 - i],
                "top effective address aligns to fffffffc");
}

static void failed_word_stores_write_nothing() {
    const auto source = distinct_vector();

    sfr::GuestMemory tail;
    tail.map(0xb0000, 3);
    std::fill_n(tail.base() + 0xb0000, 3, uint8_t(0x31));
    require_stop([&] { sfr::store_vector_word(tail, 0xb0003, source); }, "memory-access",
                 "three-byte logical tail rejects a word store");
    require(std::all_of(tail.base() + 0xb0000, tail.base() + 0xb0003,
                        [](uint8_t byte) { return byte == 0x31; }),
            "logical-tail failure writes nothing");

    sfr::GuestMemory guarded;
    guarded.map(0xc0000, 4);
    std::fill_n(guarded.base() + 0xc0000, 4, uint8_t(0x42));
    guarded.add_import_variable(0xc0000, "WordGuard");
    require_stop([&] { sfr::store_vector_word(guarded, 0xc0002, source); }, "import-variable",
                 "guard rejects a word store");
    require(std::all_of(guarded.base() + 0xc0000, guarded.base() + 0xc0004,
                        [](uint8_t byte) { return byte == 0x42; }),
            "guard failure writes nothing");

    sfr::GuestMemory read_only;
    read_only.map(0xd0000, 4);
    std::fill_n(read_only.base() + 0xd0000, 4, uint8_t(0x53));
    unsigned calls = 0;
    read_only.add_read_only_word(0xd0000, [&] { ++calls; return 0u; });
    require_stop([&] { sfr::store_vector_word(read_only, 0xd0001, source); }, "memory-readonly",
                 "read-only provider rejects a word store");
    require(calls == 0 && std::all_of(read_only.base() + 0xd0000, read_only.base() + 0xd0004,
                                     [](uint8_t byte) { return byte == 0x53; }),
            "read-only failure avoids callbacks and writes nothing");

    sfr::GuestMemory reserved;
    reserved.map(0xe0000, 8);
    std::fill_n(reserved.base() + 0xe0000, 8, uint8_t(0x64));
    reserved.load_reserved_word(0xe0004);
    require_stop([&] { sfr::store_vector_word(reserved, 0xe0003, source); }, "reservation-interference",
                 "active reservation rejects a word store");
    require(std::all_of(reserved.base() + 0xe0000, reserved.base() + 0xe0008,
                        [](uint8_t byte) { return byte == 0x64; }),
            "reservation failure writes nothing");
}

static void partial_stores_cover_every_offset_and_preserve_neighbors() {
    sfr::GuestMemory memory;
    memory.map(0x100000, 0x1000);
    const auto source = distinct_vector();
    for (uint32_t offset = 0; offset < 16; ++offset) {
        for (bool left : {false, true}) {
            std::fill_n(memory.base() + 0x10001f, 18, uint8_t{0xcc});
            const uint32_t ea = 0x100020 + offset;
            if (left) sfr::store_vector_left(memory, ea, source);
            else sfr::store_vector_right(memory, ea, source);
            for (uint32_t i = 0; i < 16; ++i) {
                const bool written = left ? i >= offset : i < offset;
                const auto expected = !written ? uint8_t{0xcc} :
                    (left ? source[15 - (i - offset)] : source[offset - i - 1]);
                require(memory.base()[0x100020 + i] == expected,
                        "partial store selects exact guest bytes in vector order for every offset");
            }
            require(memory.base()[0x10001f] == 0xcc && memory.base()[0x100030] == 0xcc &&
                    source == distinct_vector(), "partial store preserves neighbors and vector source");
        }
    }
}

static void partial_stores_handle_address_edges_and_empty_right() {
    const auto source = distinct_vector();
    sfr::GuestMemory memory;
    sfr::store_vector_right(memory, 0, source);
    sfr::store_vector_right(memory, 0xfffffff0u, source);
    memory.map(0, 16);
    sfr::store_vector_left(memory, 0, source);
    require(memory.load<uint8_t>(0) == source[15] && memory.load<uint8_t>(15) == source[0],
            "left store handles first guest address");
    sfr::store_vector_right(memory, 1, source);
    require(memory.load<uint8_t>(0) == source[0], "right store reaches first guest byte without underflow");
    memory.map(0xfffff000u, 0x1000);
    memory.store<uint8_t>(0xfffffffeu, 0xcc);
    sfr::store_vector_left(memory, 0xffffffffu, source);
    require(memory.load<uint8_t>(0xffffffffu) == source[15] && memory.load<uint8_t>(0xfffffffeu) == 0xcc,
            "left store reaches last guest byte without overflow");
    sfr::store_vector_right(memory, 0xffffffffu, source);
    require(memory.load<uint8_t>(0xfffffff0u) == source[14] &&
            memory.load<uint8_t>(0xfffffffeu) == source[0] && memory.load<uint8_t>(0xffffffffu) == source[15],
            "right store ends before last effective address");
    memory.load_reserved_word(0);
    sfr::store_vector_right(memory, 0x200000, source);
    require(memory.has_reservation() && memory.store_conditional_word(0, 7),
            "empty right store neither probes unmapped memory nor interferes with an atomic reservation");
}

static void partial_store_preflight_is_atomic() {
    const auto source = distinct_vector();
    for (bool left : {false, true}) {
        const uint32_t ea = left ? 0x110001 : 0x11000f;
        sfr::GuestMemory tail;
        tail.map(0x110000, 14);
        std::fill_n(tail.base() + 0x110000, 14, uint8_t{0x6a});
        require_stop([&] { if (left) sfr::store_vector_left(tail, ea, source);
                          else sfr::store_vector_right(tail, ea, source); }, "memory-access",
                     "partial store rejects missing logical tail");
        require(std::all_of(tail.base() + 0x110000, tail.base() + 0x11000e,
                           [](uint8_t byte) { return byte == 0x6a; }), "failed partial store preserves mapped bytes");
        for (bool provider : {false, true}) {
            sfr::GuestMemory guarded;
            guarded.map(0x110000, 16);
            std::fill_n(guarded.base() + 0x110000, 16, uint8_t{0x7b});
            unsigned calls = 0;
            // Last in write order, so per-byte-only checks would partially mutate memory.
            const uint32_t guard = left ? 0x11000c : 0x110000;
            if (provider) guarded.add_read_only_word(guard, [&] { ++calls; return 0u; });
            else guarded.add_import_variable(guard, "PartialStoreGuard");
            require_stop([&] { if (left) sfr::store_vector_left(guarded, ea, source);
                              else sfr::store_vector_right(guarded, ea, source); },
                         provider ? "memory-readonly" : "import-variable", "partial store preflights late guard");
            require(calls == 0 && std::all_of(guarded.base() + 0x110000, guarded.base() + 0x110010,
                        [](uint8_t byte) { return byte == 0x7b; }), "guard failure samples no provider and mutates no bytes");
        }
        sfr::GuestMemory atomic;
        atomic.map(0x110000, 32);
        std::fill_n(atomic.base() + 0x110000, 32, uint8_t{0x8c});
        atomic.load_reserved_word(0x110010);
        require_stop([&] { if (left) sfr::store_vector_left(atomic, ea, source);
                          else sfr::store_vector_right(atomic, ea, source); }, "reservation-interference",
                     "nonempty partial store rejects active reservation");
        require(std::all_of(atomic.base() + 0x110000, atomic.base() + 0x110020,
                           [](uint8_t byte) { return byte == 0x8c; }) &&
                atomic.has_reservation() && atomic.store_conditional_word(0x110010, 9),
                "partial store atomic rejection preserves bytes and usable reservation");
    }
    sfr::GuestMemory narrow;
    narrow.map(0x120000, 3);
    sfr::store_vector_right(narrow, 0x120003, source);
    require(narrow.load<uint8_t>(0x120000) == source[2] && narrow.load<uint8_t>(0x120002) == source[0],
            "right store checks only its touched prefix, not the whole vector block");
}

static void partial_stores_preserve_write_combined_mapping() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // This checks how a write-combined page is handled, so ask for the
    // console's mapping (off by default, see set_host_write_combining).
    sfr::GuestMemory::set_host_write_combining(true);
    struct Restore { ~Restore() { sfr::GuestMemory::set_host_write_combining(false); } } restore;
    sfr::GuestMemory memory;
    memory.map_write_combined(0x130000, 0x1000);
    const auto source = distinct_vector();
    sfr::store_vector_left(memory, 0x130003, source);
    sfr::store_vector_right(memory, 0x130023, source);
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(memory.base() + 0x130000, &info, sizeof(info)) == sizeof(info) &&
            info.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE) &&
            memory.load<uint8_t>(0x130003) == source[15] && memory.load<uint8_t>(0x13000f) == source[3] &&
            memory.load<uint8_t>(0x130020) == source[2] && memory.load<uint8_t>(0x130022) == source[0],
            "partial stores preserve native write-combined protection and write correct bytes");
#endif
}

static sfr::VectorBytes reversed_architecture(const std::array<uint8_t, 16>& architecture) {
    sfr::VectorBytes result{};
    std::reverse_copy(architecture.begin(), architecture.end(), result.begin());
    return result;
}

static void partial_loads_match_independent_architecture_slices_at_all_offsets() {
    sfr::GuestMemory memory;
    memory.map(0x140000, 0x1000);
    const std::array<uint8_t, 16> block{
        0x00, 0x80, 0xff, 0x43, 0x51, 0x62, 0x74, 0x85,
        0x96, 0xa7, 0xb8, 0xc9, 0xda, 0xeb, 0xfc, 0x1d};
    for (uint32_t i = 0; i < block.size(); ++i) memory.store<uint8_t>(0x140020 + i, block[i]);
    for (uint32_t offset = 0; offset < 16; ++offset) {
        std::array<uint8_t, 16> left_arch{}, right_arch{};
        std::copy(block.begin() + offset, block.end(), left_arch.begin());
        std::copy(block.begin(), block.begin() + offset, right_arch.begin() + 16 - offset);
        require(sfr::load_vector_left(memory, 0x140020 + offset) == reversed_architecture(left_arch),
                "left load matches architecture slice, zero padding and host reversal at every offset");
        require(sfr::load_vector_right(memory, 0x140020 + offset) == reversed_architecture(right_arch),
                "right load matches architecture slice, zero padding and host reversal at every offset");
    }
}

static void paired_partial_loads_merge_to_one_unaligned_vector() {
    sfr::GuestMemory memory;
    memory.map(0x141000, 0x1000);
    for (uint32_t i = 0; i < 48; ++i) memory.store<uint8_t>(0x141010 + i, uint8_t(i * 11 + 3));
    for (uint32_t offset = 0; offset < 16; ++offset) {
        const uint32_t ea = 0x141010 + offset;
        const auto left = sfr::load_vector_left(memory, ea);
        const auto right = sfr::load_vector_right(memory, uint32_t(ea + 16));
        sfr::VectorBytes merged{};
        std::array<uint8_t, 16> architecture{};
        for (uint32_t i = 0; i < 16; ++i) {
            merged[i] = left[i] | right[i];
            architecture[i] = memory.load<uint8_t>(uint64_t(ea) + i);
        }
        require(merged == reversed_architecture(architecture),
                "left/right pair reconstructs the independent unaligned sixteen-byte slice");
    }
}

static void partial_loads_check_exact_selected_ranges_before_provider_reads() {
    {
        sfr::GuestMemory memory;
        memory.map(0x142000, 32);
        unsigned outside = 0;
        memory.add_read_only_word(0x142000, [&] { ++outside; return 0x11223344u; });
        memory.add_import_variable(0x14201c, "LeftSelectedTail");
        auto destination = distinct_vector();
        const auto original = destination;
        require_stop([&] { destination = sfr::load_vector_left(memory, 0x142013); }, "import-variable",
                     "left load preflights its selected tail guard");
        require(destination == original && outside == 0,
                "left failure preserves destination and never samples provider before selected range");
    }
    {
        sfr::GuestMemory memory;
        memory.map(0x143000, 32);
        unsigned excluded = 0;
        memory.add_import_variable(0x143000, "RightSelectedPrefix");
        memory.add_read_only_word(0x14300c, [&] { ++excluded; return 0xaabbccddu; });
        auto destination = distinct_vector();
        const auto original = destination;
        require_stop([&] { destination = sfr::load_vector_right(memory, 0x14300b); }, "import-variable",
                     "right load preflights a selected prefix guard");
        require(destination == original && excluded == 0,
                "right failure preserves destination and ignores excluded suffix provider");
    }
    {
        sfr::GuestMemory memory;
        memory.map(0x145000, 16);
        unsigned selected_provider = 0;
        memory.add_read_only_word(0x145004, [&] { ++selected_provider; return 0x10203040u; });
        memory.add_import_variable(0x14500c, "LateSelectedGuard");
        auto destination = distinct_vector();
        const auto original = destination;
        require_stop([&] { destination = sfr::load_vector_left(memory, 0x145003); }, "import-variable",
                     "whole selected range is checked before an early provider can be sampled");
        require(destination == original && selected_provider == 0,
                "late selected guard prevents all earlier selected provider callbacks and publication");
    }
    {
        sfr::GuestMemory left;
        left.map(0x147000, 16);
        for (uint32_t i = 0; i < 16; ++i) left.store<uint8_t>(0x147000 + i, uint8_t(0x60 + i));
        left.add_import_variable(0x147000, "ExcludedLeftPrefix");
        require(sfr::load_vector_left(left, 0x147004)[15] == 0x64,
                "left load succeeds with a same-block guard wholly before its selected range");

        sfr::GuestMemory right;
        right.map(0x148000, 16);
        unsigned excluded_provider = 0;
        right.add_read_only_word(0x14800c, [&] { ++excluded_provider; return 0x11223344u; });
        require(sfr::load_vector_right(right, 0x14800c)[0] == right.load<uint8_t>(0x14800b) &&
                    excluded_provider == 0,
                "right load succeeds without sampling a same-block excluded suffix provider");
    }
    {
        sfr::GuestMemory memory;
        memory.map(0x146000, 16);
        unsigned excluded_provider = 0;
        memory.add_read_only_word(0x146004, [&] { ++excluded_provider; return 0x55667788u; });
        memory.add_import_variable(0x14600c, "AlignedRightExcludedGuard");
        require(sfr::load_vector_right(memory, 0x146000) == sfr::VectorBytes{} &&
                    excluded_provider == 0,
                "aligned right performs no check, guard lookup, or provider callback in its block");
    }
}

static void partial_load_edges_and_empty_right_preserve_reservations() {
    sfr::GuestMemory memory;
    memory.map(0, 0x1000);
    memory.map(0xfffff000u, 0x1000);
    memory.store<uint8_t>(0xffffffffu, 0x5a);
    memory.load_reserved_word(0);
    require(sfr::load_vector_left(memory, 0xffffffffu)[15] == 0x5a,
            "left load reaches exactly the final guest byte");
    require(memory.has_reservation(), "successful partial load preserves reservation");
    const auto right_top = sfr::load_vector_right(memory, 0xffffffffu);
    require(right_top[0] == memory.load<uint8_t>(0xfffffffeu) && memory.has_reservation(),
            "right top-address load excludes effective address and preserves reservation");
    sfr::GuestMemory unmapped;
    require(sfr::load_vector_right(unmapped, 0) == sfr::VectorBytes{} &&
                sfr::load_vector_right(unmapped, 0xfffffff0u) == sfr::VectorBytes{},
            "aligned right returns zero without probing unmapped memory");
    require(memory.store_conditional_word(0, memory.load<uint32_t>(0)),
            "partial loads leave the original reservation usable");

    sfr::GuestMemory wrapped;
    wrapped.map(0, 0x1000);
    wrapped.store<uint8_t>(0x10, 0x91);
    const uint32_t wrapped_ea = uint32_t(uint32_t(0xfffffff0u) + uint32_t(0x20));
    require(wrapped_ea == 0x10 && sfr::load_vector_left(wrapped, wrapped_ea)[15] == 0x91,
            "already wrapped uint32 effective address selects low guest memory");

    sfr::GuestMemory read_only;
    read_only.map(0x144000, 16);
    unsigned samples = 0;
    read_only.add_read_only_word(0x144004, [&] { ++samples; return 0x10203040u; });
    const auto loaded = sfr::load_vector_left(read_only, 0x144003);
    require(samples == 4 && loaded[11] == 0x40 && loaded[14] == 0x10,
            "selected read-only provider remains readable through checked byte loads");
}

int main() {
    try {
        for (auto test : {load_reverses_guest_block_for_every_effective_address_offset,
                          store_roundtrip_preserves_neighbors_and_source,
                          store_aligns_down_for_every_effective_address_offset,
                          top_address_aligns_to_last_complete_block,
                          complete_range_is_checked_before_reading,
                          complete_store_range_is_checked_before_writing,
                          complete_load_check_precedes_provider_sampling,
                          computed_words_are_sampled_once_per_containing_word,
                          provider_exception_in_second_half_does_not_replace_destination,
                          failed_stores_do_not_partially_write,
                          word_store_selects_by_aligned_effective_address,
                          word_store_checks_only_its_aligned_word,
                          failed_word_stores_write_nothing,
                          partial_stores_cover_every_offset_and_preserve_neighbors,
                          partial_stores_handle_address_edges_and_empty_right,
                          partial_store_preflight_is_atomic,
                          partial_stores_preserve_write_combined_mapping,
                          partial_loads_match_independent_architecture_slices_at_all_offsets,
                          paired_partial_loads_merge_to_one_unaligned_vector,
                          partial_loads_check_exact_selected_ranges_before_provider_reads,
                          partial_load_edges_and_empty_right_preserve_reservations})
            test();
        std::cout << "Vector memory checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
