#include "ansi_string.h"
#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename F>
void require_stop(F operation, const char* category, const char* message) {
    try { operation(); }
    catch (const sfr::RuntimeStop& error) {
        require(error.category == category, message);
        return;
    }
    throw std::runtime_error(message);
}

std::array<uint8_t, 8> bytes_at(const sfr::GuestMemory& memory, uint32_t address) {
    std::array<uint8_t, 8> bytes{};
    std::copy_n(memory.base() + address, bytes.size(), bytes.begin());
    return bytes;
}

void exact_descriptor_aliases_and_preserves_source() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x1000);
    constexpr uint32_t source = 0x10100;
    constexpr uint32_t destination = 0x10200;
    const std::array<uint8_t, 5> text{'A', 0x80, 0xff, 'Z', 0};
    for (size_t i = 0; i < text.size(); ++i) memory.store<uint8_t>(source + i, text[i]);
    memory.store<uint64_t>(destination, 0xa1a2a3a4a5a6a7a8ull);

    sfr::initialize_ansi_string(memory, destination, source);

    const std::array<uint8_t, 8> expected{0x00, 0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00};
    require(bytes_at(memory, destination) == expected, "descriptor is exact big-endian counted ANSI_STRING");
    for (size_t i = 0; i < text.size(); ++i)
        require(memory.load<uint8_t>(source + i) == text[i], "initializer preserves source bytes");
}

void null_and_non_null_empty_are_distinct() {
    sfr::GuestMemory memory;
    memory.map(0x20000, 0x1000);
    memory.store<uint64_t>(0x20000, UINT64_MAX);
    sfr::initialize_ansi_string(memory, 0x20000, 0);
    require(bytes_at(memory, 0x20000) == std::array<uint8_t, 8>{}, "null source produces an all-zero descriptor");

    memory.store<uint8_t>(0x20100, 0);
    sfr::initialize_ansi_string(memory, 0x20000, 0x20100);
    const std::array<uint8_t, 8> expected{0, 0, 0, 1, 0, 2, 1, 0};
    require(bytes_at(memory, 0x20000) == expected, "non-null empty source retains pointer and one-byte maximum");
}

void nul_at_last_mapped_byte_succeeds() {
    sfr::GuestMemory memory;
    memory.map(0x30000, 0x1000);
    memory.map(0x40000, 3);
    memory.store<uint8_t>(0x40000, 'x');
    memory.store<uint8_t>(0x40001, 'y');
    memory.store<uint8_t>(0x40002, 0);
    sfr::initialize_ansi_string(memory, 0x30000, 0x40000);
    require(memory.load<uint16_t>(0x30000) == 2 && memory.load<uint16_t>(0x30002) == 3 &&
            memory.load<uint32_t>(0x30004) == 0x40000,
            "terminator at final mapped byte is accepted");
}

void bounded_lengths_are_enforced_without_mutation() {
    auto run = [](uint32_t length, bool succeeds) {
        sfr::GuestMemory memory;
        memory.map(0x50000, 8);
        memory.map(0x60000, uint64_t(length) + 1);
        std::fill_n(memory.base() + 0x60000, length, uint8_t{0x81});
        memory.base()[0x60000 + length] = 0;
        const std::array<uint8_t, 8> sentinel{0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
        std::copy(sentinel.begin(), sentinel.end(), memory.base() + 0x50000);
        if (succeeds) {
            sfr::initialize_ansi_string(memory, 0x50000, 0x60000);
            require(memory.load<uint16_t>(0x50000) == 65534 && memory.load<uint16_t>(0x50002) == 65535,
                    "largest representable length succeeds");
        } else {
            require_stop([&] { sfr::initialize_ansi_string(memory, 0x50000, 0x60000); },
                         "ansi-string-length", "65535-byte ANSI string is explicitly unsupported");
            require(bytes_at(memory, 0x50000) == sentinel, "length rejection preserves descriptor");
        }
    };
    run(65534, true);
    run(65535, false);
}

void failed_validation_preserves_destination() {
    const std::array<uint8_t, 8> sentinel{0xde, 0xad, 0xbe, 0xef, 0x12, 0x34, 0x56, 0x78};
    auto reject = [&](uint32_t source, auto prepare, const char* category, const char* message) {
        sfr::GuestMemory memory;
        memory.map(0x80000, 8);
        std::copy(sentinel.begin(), sentinel.end(), memory.base() + 0x80000);
        prepare(memory);
        require_stop([&] { sfr::initialize_ansi_string(memory, 0x80000, source); }, category, message);
        require(bytes_at(memory, 0x80000) == sentinel, "source failure preserves descriptor");
    };
    reject(0x90000, [](sfr::GuestMemory&) {}, "memory-access", "missing source is rejected");
    reject(0x90000, [](sfr::GuestMemory& m) { m.map(0x90000, 3); m.store<uint8_t>(0x90000, 'a');
            m.store<uint8_t>(0x90001, 'b'); m.store<uint8_t>(0x90002, 'c'); },
           "memory-access", "unterminated truncated source is rejected");
    reject(0xfffffffeu, [](sfr::GuestMemory& m) { m.map(0xfffff000, 0x1000);
            m.store<uint8_t>(0xfffffffeu, 'a'); m.store<uint8_t>(0xffffffffu, 'b'); },
           "ansi-string-address", "source scan cannot wrap the 4 GiB address space");

    sfr::GuestMemory memory;
    memory.map(0xa0000, 7);
    memory.map(0xb0000, 1);
    memory.store<uint8_t>(0xb0000, 0);
    require_stop([&] { sfr::initialize_ansi_string(memory, 0xa0000, 0xb0000); },
                 "memory-access", "full destination is validated before source use");
}

void readonly_destination_is_rejected_atomically() {
    sfr::GuestMemory memory;
    memory.map(0xc0000, 0x1000);
    memory.store<uint64_t>(0xc0000, 0x1122334455667788ull);
    memory.store<uint8_t>(0xc0100, 'x');
    memory.store<uint8_t>(0xc0101, 0);
    memory.add_read_only_word(0xc0004, [] { return 0x55667788u; });
    require_stop([&] { sfr::initialize_ansi_string(memory, 0xc0000, 0xc0100); },
                 "memory-readonly", "read-only destination is rejected");
    require(memory.base()[0xc0000] == 0x11 && memory.base()[0xc0007] == 0x88,
            "read-only destination rejection preserves backing");
}
}

int main() {
    try {
        exact_descriptor_aliases_and_preserves_source();
        null_and_non_null_empty_are_distinct();
        nul_at_last_mapped_byte_succeeds();
        bounded_lengths_are_enforced_without_mutation();
        failed_validation_preserves_destination();
        readonly_destination_is_rejected_atomically();
        std::cout << "ANSI string checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
