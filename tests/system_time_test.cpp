#include "system_time.h"
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

// Read only committed backing bytes, including after adding an import guard.
static std::vector<uint8_t> bytes(const sfr::GuestMemory& memory, uint64_t address, size_t size) {
    return {memory.base() + address, memory.base() + address + size};
}

static void unix_epoch_write() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 8);
    sfr::write_system_time(memory, 0x10000, 0);
    const std::vector<uint8_t> actual(memory.base() + 0x10000, memory.base() + 0x10008);
    require(actual == std::vector<uint8_t>({0x01, 0x9d, 0xb1, 0xde, 0xd5, 0x3e, 0x80, 0x00}),
            "Unix epoch writer must produce exact FILETIME epoch bytes");
}

static void conversion_boundaries() {
    struct Case { int64_t ticks; uint64_t expected; };
    for (auto value : {Case{-116444736000000000ll, 0},
                       Case{-1, 116444735999999999ull},
                       Case{0, 116444736000000000ull},
                       Case{1, 116444736000000001ull},
                       Case{9466848000000000ll, 125911584000000000ull},
                       Case{std::numeric_limits<int64_t>::max(), 9339816772854775807ull}}) {
        require(sfr::filetime_from_unix_ticks(value.ticks) == value.expected, "FILETIME conversion boundary");
        sfr::GuestMemory memory;
        memory.map(0x10000, 8);
        sfr::write_system_time(memory, 0x10000, value.ticks);
        require(memory.load<uint64_t>(0x10000) == value.expected, "writer conversion boundary");
    }
}

static void rejected_times_preserve_bytes() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 8);
    memory.store<uint64_t>(0x10000, 0x123456789abcdef0ull);
    const auto before = bytes(memory, 0x10000, 8);
    for (int64_t ticks : std::array<int64_t, 2>{-116444736000000001ll, std::numeric_limits<int64_t>::min()}) {
        rejects([&] { sfr::filetime_from_unix_ticks(ticks); }, "time-range");
        rejects([&] { sfr::write_system_time(memory, 0x10000, ticks); }, "time-range");
        require(bytes(memory, 0x10000, 8) == before, "invalid time preserves destination");
        rejects([&] { sfr::write_system_time(memory, 0x20000, ticks); }, "time-range");
    }
}

static void null_is_noop() {
    sfr::GuestMemory memory;
    // Null remains a no-op even when address zero has real, guarded backing.
    memory.map(0, 8);
    memory.store<uint64_t>(0, 0x123456789abcdef0ull);
    const auto before = bytes(memory, 0, 8);
    memory.add_import_variable(0, "null output guard");
    sfr::write_system_time(memory, 0, 0);
    sfr::write_system_time(memory, 0, std::numeric_limits<int64_t>::min());
    sfr::query_system_time(memory, 0);
    require(bytes(memory, 0, 8) == before, "null output must not touch guest memory");
}

static void exact_unaligned_and_cross_page_write() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 0x2000);
    for (uint32_t address : {0x10001u, 0x10ffdu}) {
        for (uint32_t i = address - 1; i <= address + 8; ++i) memory.store<uint8_t>(i, 0xa5);
        sfr::write_system_time(memory, address, 1);
        require(bytes(memory, address - 1, 10) ==
                    std::vector<uint8_t>({0xa5, 1, 0x9d, 0xb1, 0xde, 0xd5, 0x3e, 0x80, 1, 0xa5}),
                "writer changes exactly eight big endian bytes");
    }
}

static void output_failures_preserve_bytes() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 7);
    memory.map(0xfffff000, 0x1000);
    memory.map(0x20000, 12);
    for (uint32_t i = 0; i < 7; ++i) memory.store<uint8_t>(0x10000 + i, 0xa5);
    memory.store<uint32_t>(0xfffffffc, 0x12345678);
    memory.store<uint64_t>(0x20000, 0x123456789abcdef0ull);
    memory.store<uint32_t>(0x20008, 0x12345678);
    const auto tail = bytes(memory, 0x10000, 7);
    const auto wrap = bytes(memory, 0xfffffffc, 4);
    const auto guarded = bytes(memory, 0x20000, 12);
    memory.add_import_variable(0x20007, "last byte output guard");
    struct Case { uint32_t address; const char* category; };
    for (auto value : {Case{0x30000, "memory-access"}, Case{0x10000, "memory-access"},
                       Case{0xfffffffc, "memory-access"}, Case{0x20000, "import-variable"}}) {
        rejects([&] { sfr::write_system_time(memory, value.address, 0); }, value.category);
        rejects([&] { sfr::query_system_time(memory, value.address); }, value.category);
        require(bytes(memory, 0x10000, 7) == tail, "invalid tail preserves prefix");
        require(bytes(memory, 0xfffffffc, 4) == wrap, "wrapping write preserves prefix");
        require(bytes(memory, 0x20000, 12) == guarded, "guarded tail preserves all bytes");
    }
}

static void real_query_tracks_host_time() {
    sfr::GuestMemory memory;
    memory.map(0x10000, 10);
    memory.store<uint8_t>(0x10000, 0xa5);
    memory.store<uint8_t>(0x10009, 0x5a);
    const auto before = std::chrono::system_clock::now();
    sfr::query_system_time(memory, 0x10001);
    const auto after = std::chrono::system_clock::now();
    // Independent millisecond samples avoid mirroring production's 100ns conversion.
    // Allow ten seconds for coarse clocks or a modest wall-clock adjustment.
    const auto before_ms = std::chrono::duration_cast<std::chrono::milliseconds>(before.time_since_epoch()).count();
    const auto after_ms = std::chrono::duration_cast<std::chrono::milliseconds>(after.time_since_epoch()).count();
    const uint64_t lower = 116444736000000000ull + (before_ms - 10000) * 10000;
    const uint64_t upper = 116444736000000000ull + (after_ms + 10000) * 10000;
    const uint64_t actual = memory.load<uint64_t>(0x10001);
    require(actual >= lower && actual <= upper, "query must return current host wall time in FILETIME ticks");
    require(memory.load<uint8_t>(0x10000) == 0xa5 && memory.load<uint8_t>(0x10009) == 0x5a,
            "real query preserves neighboring bytes");
}

int main() {
    struct Test { const char* name; void (*run)(); };
    int failures = 0;
    for (auto test : {Test{"Unix epoch writer", unix_epoch_write},
                      Test{"conversion boundaries", conversion_boundaries},
                      Test{"rejected times preserve bytes", rejected_times_preserve_bytes},
                      Test{"null no-op", null_is_noop},
                      Test{"exact unaligned and cross-page write", exact_unaligned_and_cross_page_write},
                      Test{"output failures preserve bytes", output_failures_preserve_bytes},
                      Test{"real host query", real_query_tracks_host_time}}) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    if (failures) return 1;
    std::cout << "System time checks passed (7 groups)\n";
}
