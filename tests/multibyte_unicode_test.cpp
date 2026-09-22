#include "guest_memory.h"
#include "multibyte_unicode.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename F> static void rejects(F&& operation, const char* message) {
    try { operation(); }
    catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error(message);
}

static void map_fixture(sfr::GuestMemory& memory) {
    memory.map(0x10000, 0x4000);
    for (uint32_t i = 0; i < 0x4000; ++i) memory.store<uint8_t>(0x10000 + i, 0xA5);
}

static void exact_ascii_and_nuls() {
    sfr::GuestMemory memory;
    map_fixture(memory);
    memory.store<uint8_t>(0x11000, 0);
    require(sfr::multibyte_to_unicode_ascii(memory, 0x12000, 32, 0, 0x11000, 1) == 0,
            "single NUL conversion succeeds");
    require(memory.load<uint16_t>(0x12000) == 0 && memory.load<uint8_t>(0x12002) == 0xA5,
            "single NUL writes one BE16 unit and no terminator");

    for (uint32_t i = 0; i < 128; ++i) memory.store<uint8_t>(0x11000 + i, uint8_t(i));
    require(sfr::multibyte_to_unicode_ascii(memory, 0x12000, 256, 0x13000, 0x11000, 128) == 0,
            "complete ASCII range converts");
    require(memory.load<uint32_t>(0x13000) == 256, "full conversion reports output bytes");
    for (uint32_t i = 0; i < 128; ++i)
        require(memory.load<uint16_t>(0x12000 + i * 2) == i, "ASCII unit is exact BE16");

    memory.store<uint8_t>(0x11000, 'A');
    memory.store<uint8_t>(0x11001, 0);
    memory.store<uint8_t>(0x11002, 'B');
    require(sfr::multibyte_to_unicode_ascii(memory, 0x12000, 6, 0x13000, 0x11000, 3) == 0 &&
            memory.load<uint16_t>(0x12000) == 'A' && memory.load<uint16_t>(0x12002) == 0 &&
            memory.load<uint16_t>(0x12004) == 'B' && memory.load<uint32_t>(0x13000) == 6,
            "embedded NUL does not terminate counted input");
}

static void truncation_and_zero_work() {
    sfr::GuestMemory memory;
    map_fixture(memory);
    memory.store<uint8_t>(0x11000, 'A');
    memory.store<uint8_t>(0x11001, 'B');
    memory.store<uint8_t>(0x11002, 0xFF);
    for (uint32_t capacity : {0u, 1u, 3u, 5u}) {
        memory.store<uint32_t>(0x13000, 0xCCCCCCCC);
        memory.store<uint64_t>(0x12000, 0xA5A5A5A5A5A5A5A5ull);
        require(sfr::multibyte_to_unicode_ascii(memory, 0x12000, capacity, 0x13000, 0x11000, 3) == 0,
                "truncated conversion succeeds");
        const uint32_t units = capacity / 2;
        require(memory.load<uint32_t>(0x13000) == units * 2, "truncation reports actual byte count");
        if (units) require(memory.load<uint16_t>(0x12000) == 'A', "selected prefix converted");
        if (units > 1) require(memory.load<uint16_t>(0x12002) == 'B', "second selected byte converted");
        require(memory.load<uint8_t>(0x12000 + units * 2) == 0xA5, "unused capacity remains untouched");
    }
    memory.store<uint32_t>(0x13000, 1);
    require(sfr::multibyte_to_unicode_ascii(memory, 0xFFFFFFFF, 0, 0x13000, 0xFFFFFFFF, 0xFFFFFFFF) == 0 &&
            memory.load<uint32_t>(0x13000) == 0,
            "zero capacity neither reads source nor writes destination");
}

static void guards_encoding_and_atomicity() {
    sfr::GuestMemory memory;
    map_fixture(memory);
    memory.store<uint8_t>(0x11000, 'A');
    memory.store<uint8_t>(0x11001, 0x80);
    memory.store<uint64_t>(0x12000, 0x1122334455667788ull);
    memory.store<uint32_t>(0x13000, 0x99AABBCC);
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x12000, 4, 0x13000, 0x11000, 2); },
            "consumed high byte is unsupported");
    require(memory.load<uint64_t>(0x12000) == 0x1122334455667788ull &&
            memory.load<uint32_t>(0x13000) == 0x99AABBCC, "late high byte leaves outputs intact");
    require(sfr::multibyte_to_unicode_ascii(memory, 0x12000, 2, 0x13000, 0x11000, 2) == 0,
            "unconsumed high byte is ignored");

    memory.store<uint64_t>(0x12000, 0x1122334455667788ull);
    memory.store<uint32_t>(0x13000, 0x99AABBCC);
    memory.add_read_only_word(0x13000, [] { return 0x99AABBCCu; });
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x12000, 2, 0x13000, 0x11000, 1); },
            "count guard is preflighted");
    require(memory.load<uint64_t>(0x12000) == 0x1122334455667788ull,
            "count guard prevents earlier destination publication");

    sfr::GuestMemory provider;
    provider.map(0x20000, 0x1000);
    provider.add_read_only_word(0x20000, [] { return 0x41424344u; });
    provider.map(0x22000, 0x1000);
    require(sfr::multibyte_to_unicode_ascii(provider, 0x22000, 8, 0, 0x20000, 4) == 0 &&
            provider.load<uint64_t>(0x22000) == 0x0041004200430044ull,
            "computed source is sampled through normal guest reads");
    rejects([&] { sfr::multibyte_to_unicode_ascii(provider, 0x22000, 4, 0, 0x20FFF, 2); },
            "entire selected source range is validated");
}

static void aliases_bounds_and_reservation() {
    sfr::GuestMemory memory;
    map_fixture(memory);
    memory.store<uint32_t>(0x11000, 0x41424344);
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x11000, 8, 0, 0x11000, 4); },
            "source and destination alias is rejected");
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x12000, 8, 0x11002, 0x11000, 4); },
            "count and input alias is rejected");
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x12000, 8, 0x12002, 0x11000, 4); },
            "count and output alias is rejected");
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0xFFFFFFFE, 4, 0, 0x11000, 2); },
            "destination wrap is rejected");
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x12000, 4, 0, 0xFFFFFFFF, 2); },
            "source wrap is rejected");
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x12000, 0x200002, 0, 0x11000, 0x100001); },
            "conversion work is bounded");

    memory.store<uint32_t>(0x11000, 0x41424344);
    memory.load_reserved_word(0x11000);
    rejects([&] { sfr::multibyte_to_unicode_ascii(memory, 0x12000, 8, 0x13004, 0x11000, 4); },
            "active reservation rejects output publication");
    require(memory.store_conditional_word(0x11000, 0x45464748),
            "failed conversion preserves reservation usability");
}

int main() {
    try {
        exact_ascii_and_nuls();
        truncation_and_zero_work();
        guards_encoding_and_atomicity();
        aliases_bounds_and_reservation();
        std::cout << "multibyte unicode tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
