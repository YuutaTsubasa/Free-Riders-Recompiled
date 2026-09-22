#include "video_mode.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
static void stops(Operation operation, const char* category) {
    try { operation(); }
    catch (const sfr::RuntimeStop& stop) {
        require(stop.category == category, "unexpected stop category");
        return;
    }
    throw std::runtime_error(std::string("expected stop: ") + category);
}

static std::vector<uint8_t> bytes(const sfr::GuestMemory& memory, uint64_t address, size_t size) {
    return {memory.base() + address, memory.base() + address + size};
}

static void exact_protocol_layout() {
    sfr::GuestMemory memory;
    constexpr uint32_t output = 0x10004;
    memory.map(0x10000, 56);
    memory.store<uint32_t>(output - 4, 0x11223344);
    memory.store<uint32_t>(output + 48, 0x55667788);
    sfr::VideoMode mode(memory, {1920, 1080, 59.94f, false});
    const std::array<uint32_t, 12> expected{
        1920, 1080, 0, 1, 1, std::bit_cast<uint32_t>(59.94f), 1, 0x4a, 1, 0, 0, 0};
    require(mode.snapshot() == expected, "snapshot exposes the twelve protocol words");
    mode.query(output);
    for (size_t i = 0; i < expected.size(); ++i)
        require(memory.load<uint32_t>(uint64_t(output) + i * 4) == expected[i], "query stores every big-endian word");
    require(memory.base()[output] == 0 && memory.base()[output + 1] == 0 &&
            memory.base()[output + 2] == 7 && memory.base()[output + 3] == 0x80,
            "width bytes are big endian");
    require(memory.load<uint32_t>(output - 4) == 0x11223344 &&
            memory.load<uint32_t>(output + 48) == 0x55667788, "query preserves neighbors");
}

static void derived_flags() {
    sfr::GuestMemory memory;
    for (auto value : std::array<std::pair<sfr::DisplayMode, std::array<uint32_t, 2>>, 4>{
             std::pair{sfr::DisplayMode{640, 480, 60.0f, false}, std::array<uint32_t, 2>{0, 0}},
             std::pair{sfr::DisplayMode{1024, 768, 75.0f, false}, std::array<uint32_t, 2>{0, 1}},
             std::pair{sfr::DisplayMode{1280, 720, 120.0f, false}, std::array<uint32_t, 2>{1, 1}},
             std::pair{sfr::DisplayMode{1280, 960, 30.0f, false}, std::array<uint32_t, 2>{0, 1}}}) {
        const auto words = sfr::VideoMode(memory, value.first).snapshot();
        require(words[3] == value.second[0] && words[4] == value.second[1], "aspect and HD flags use exact boundaries");
        require(words[5] == std::bit_cast<uint32_t>(value.first.refresh_hz), "refresh keeps exact float bits");
    }
}

static void invalid_modes_stop() {
    sfr::GuestMemory memory;
    for (const auto mode : std::array<sfr::DisplayMode, 10>{
             sfr::DisplayMode{0, 1080, 60, false}, {16385, 1080, 60, false},
             {1920, 0, 60, false}, {1920, 16385, 60, false},
             {1920, 1080, 1, false}, {1920, 1080, 0, false},
             {1920, 1080, 1000.01f, false}, {1920, 1080, std::numeric_limits<float>::infinity(), false},
             {1920, 1080, std::numeric_limits<float>::quiet_NaN(), false}, {1920, 1080, 60, true}})
        stops([&] { sfr::VideoMode invalid(memory, mode); }, "video-mode");
    sfr::VideoMode(memory, {1, 1, std::nextafter(1.0f, 2.0f), false});
    sfr::VideoMode(memory, {16384, 16384, 1000.0f, false});
}

static void output_failures_are_atomic() {
    sfr::GuestMemory memory;
    sfr::VideoMode mode(memory, {1920, 1080, 60, false});
    stops([&] { mode.query(0); }, "video-mode");
    stops([&] { mode.query(0x20000); }, "memory-access");

    memory.map(0x30000, 47);
    std::fill_n(memory.base() + 0x30000, 47, 0xa5);
    const auto tail = bytes(memory, 0x30000, 47);
    stops([&] { mode.query(0x30000); }, "memory-access");
    require(bytes(memory, 0x30000, 47) == tail, "partial mapped tail receives no writes");

    memory.map(0x40000, 64);
    std::fill_n(memory.base() + 0x40000, 64, 0xb6);
    memory.add_read_only_word(0x4002c, [] { return 0u; });
    const auto read_only = bytes(memory, 0x40000, 64);
    stops([&] { mode.query(0x40000); }, "memory-readonly");
    require(bytes(memory, 0x40000, 64) == read_only, "late read-only word prevents all writes");

    memory.map(0x50000, 64);
    std::fill_n(memory.base() + 0x50000, 64, 0xc7);
    memory.add_import_variable(0x5002c, "video mode guard");
    const auto guarded = bytes(memory, 0x50000, 64);
    stops([&] { mode.query(0x50000); }, "import-variable");
    require(bytes(memory, 0x50000, 64) == guarded, "late guard prevents all writes");

    memory.map(0xfffff000, 0x1000);
    memory.store<uint32_t>(0xfffffffc, 0x12345678);
    stops([&] { mode.query(0xfffffffc); }, "memory-access");
    require(memory.load<uint32_t>(0xfffffffc) == 0x12345678, "wrapping destination remains unchanged");

    memory.map(0x60000, 64);
    std::fill_n(memory.base() + 0x60000, 64, 0xd8);
    memory.load_reserved_word(0x60030);
    const auto reserved = bytes(memory, 0x60000, 64);
    stops([&] { mode.query(0x60000); }, "reservation-interference");
    require(bytes(memory, 0x60000, 64) == reserved && memory.has_reservation(),
            "live reservation prevents all writes and remains live");
}

int main() {
    struct Test { const char* name; void (*run)(); };
    int failures = 0;
    for (const auto test : {Test{"exact protocol layout", exact_protocol_layout},
                            Test{"derived flags", derived_flags},
                            Test{"invalid modes", invalid_modes_stop},
                            Test{"output atomicity", output_failures_are_atomic}}) {
        try { test.run(); }
        catch (const std::exception& error) { std::cerr << test.name << ": " << error.what() << '\n'; ++failures; }
    }
    if (failures) return 1;
    std::cout << "Video mode checks passed (4 groups)\n";
}
