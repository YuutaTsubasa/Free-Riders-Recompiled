#include "video_globals.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
bool stops(Operation operation, const char* category) {
    try { operation(); }
    catch (const sfr::RuntimeStop& error) { return error.category == category; }
    return false;
}

void device_cell_is_exact_writable_storage() {
    sfr::GuestMemory memory;
    sfr::VideoGlobals globals(memory);
    require(sfr::VideoGlobals::device_cell % 32 == 0, "video device cell has observed 32-byte alignment");
    require(memory.load<uint32_t>(sfr::VideoGlobals::device_cell) == 0,
            "video device cell starts as a real zero word");
    memory.store<uint32_t>(sfr::VideoGlobals::device_cell, 0x12345678);
    require(memory.load<uint32_t>(sfr::VideoGlobals::device_cell) == 0x12345678 &&
            memory.base()[sfr::VideoGlobals::device_cell] == 0x12 &&
            memory.base()[sfr::VideoGlobals::device_cell + 3] == 0x78,
            "video device cell is writable big-endian storage");
    require(stops([&] { (void)memory.load<uint8_t>(sfr::VideoGlobals::device_cell + 4); }, "memory-access"),
            "video device cell has exact four-byte logical bounds");
}

void occupied_device_cell_is_rejected_without_overwrite() {
    sfr::GuestMemory memory;
    memory.map(sfr::VideoGlobals::device_cell, 4);
    memory.store<uint32_t>(sfr::VideoGlobals::device_cell, 0xdecafbad);
    require(stops([&] { sfr::VideoGlobals globals(memory); }, "memory-map"),
            "duplicate video device cell mapping is rejected");
    require(memory.load<uint32_t>(sfr::VideoGlobals::device_cell) == 0xdecafbad,
            "duplicate mapping failure preserves occupied bytes");
}

void shared_surface_returns_cell_without_touching_state() {
    sfr::GuestMemory memory;
    sfr::VideoGlobals globals(memory);
    constexpr uint32_t resource = 0x10000;
    memory.map(resource, 8);
    memory.store<uint32_t>(resource, 0x40000004);
    memory.store<uint32_t>(resource + 4, 0);
    memory.store<uint32_t>(sfr::VideoGlobals::device_cell, 0xfedcba98);
    std::array<uint8_t, 8> before{};
    std::copy_n(memory.base() + resource, before.size(), before.begin());

    struct Context { uint32_t function, lr, caller; };
    constexpr Context wrong_contexts[] = {
        {sfr::VideoGlobals::shared_function + 4, sfr::VideoGlobals::shared_lr,
         sfr::VideoGlobals::shared_caller},
        {sfr::VideoGlobals::shared_function, sfr::VideoGlobals::shared_lr + 4,
         sfr::VideoGlobals::shared_caller},
        {sfr::VideoGlobals::shared_function, sfr::VideoGlobals::shared_lr,
         sfr::VideoGlobals::shared_caller + 4},
    };
    for (const auto& context : wrong_contexts) {
        require(stops([&] { (void)globals.device_cell_for_shared_surface(
                                  resource, context.function, context.lr, context.caller); },
                      "video-globals"),
                "shared surface device-cell lookup rejects an unknown caller context");
        require(std::equal(before.begin(), before.end(), memory.base() + resource) &&
                memory.load<uint32_t>(sfr::VideoGlobals::device_cell) == 0xfedcba98,
                "context rejection preserves resource header and device cell");
    }

    require(globals.device_cell_for_shared_surface(resource, sfr::VideoGlobals::shared_function,
                                                    sfr::VideoGlobals::shared_lr,
                                                    sfr::VideoGlobals::shared_caller) == sfr::VideoGlobals::device_cell,
            "valid shared surface returns the fixed device-cell address");
    require(memory.load<uint32_t>(sfr::VideoGlobals::device_cell) == 0xfedcba98,
            "shared surface lookup ignores and preserves the current cell value");
    require(std::equal(before.begin(), before.end(), memory.base() + resource),
            "shared surface lookup does not mutate its header");
}

void invalid_shared_surfaces_stop_atomically() {
    constexpr uint32_t resource = 0x30000;
    const std::array<std::array<uint32_t, 2>, 3> invalid{{
        {{0x40000003, 0}},
        {{0x4000000a, 0}},
        {{0x40000004, 1}},
    }};
    for (const auto& values : invalid) {
        sfr::GuestMemory memory;
        sfr::VideoGlobals globals(memory);
        memory.map(resource, 8);
        memory.store<uint32_t>(resource, values[0]);
        memory.store<uint32_t>(resource + 4, values[1]);
        memory.store<uint32_t>(sfr::VideoGlobals::device_cell, 0xa5a5a5a5);
        std::array<uint8_t, 8> before{};
        std::copy_n(memory.base() + resource, before.size(), before.begin());
        require(stops([&] { (void)globals.device_cell_for_shared_surface(
                                  resource, sfr::VideoGlobals::shared_function,
                                  sfr::VideoGlobals::shared_lr, sfr::VideoGlobals::shared_caller); },
                      "video-globals"),
                "invalid shared surface metadata stops explicitly");
        require(std::equal(before.begin(), before.end(), memory.base() + resource) &&
                memory.load<uint32_t>(sfr::VideoGlobals::device_cell) == 0xa5a5a5a5,
                "invalid shared surface lookup preserves header and device cell");
    }

    // Plain resources (types 1..9 without bit 30) are accepted only
    // without a GPU fence.
    for (uint32_t kind : {1u, 2u, 3u, 5u, 7u, 9u})
    for (uint32_t fence : {0u, 1u}) {
        sfr::GuestMemory memory;
        sfr::VideoGlobals globals(memory);
        memory.map(resource, 12);
        memory.store<uint32_t>(resource, 0x00100000 | kind);
        memory.store<uint32_t>(resource + 4, 0);
        memory.store<uint32_t>(resource + 8, fence);
        auto lookup = [&] { return globals.device_cell_for_shared_surface(
                                resource, sfr::VideoGlobals::shared_function,
                                sfr::VideoGlobals::shared_lr, sfr::VideoGlobals::shared_caller); };
        if (fence == 0) require(lookup() == sfr::VideoGlobals::device_cell, "released resource without a fence");
        else require(stops([&] { (void)lookup(); }, "video-globals"), "fenced resource stops explicitly");
    }

    for (uint32_t address : {0u, resource + 1}) {
        sfr::GuestMemory memory;
        sfr::VideoGlobals globals(memory);
        require(stops([&] { (void)globals.device_cell_for_shared_surface(
                                  address, sfr::VideoGlobals::shared_function,
                                  sfr::VideoGlobals::shared_lr, sfr::VideoGlobals::shared_caller); },
                      "video-globals"),
                "null or unaligned shared surface stops explicitly");
    }
    sfr::GuestMemory truncated;
    sfr::VideoGlobals globals(truncated);
    truncated.map(resource, 7);
    std::fill_n(truncated.base() + resource, 7, uint8_t{0x5c});
    require(stops([&] { (void)globals.device_cell_for_shared_surface(
                              resource, sfr::VideoGlobals::shared_function,
                              sfr::VideoGlobals::shared_lr, sfr::VideoGlobals::shared_caller); },
                  "memory-access"),
            "truncated shared surface header fails complete preflight");
    require(std::all_of(truncated.base() + resource, truncated.base() + resource + 7,
                        [](uint8_t value) { return value == 0x5c; }),
            "truncated header rejection performs no partial writes");
}

}

int main() {
    try {
        device_cell_is_exact_writable_storage();
        occupied_device_cell_is_rejected_without_overwrite();
        shared_surface_returns_cell_without_touching_state();
        invalid_shared_surfaces_stop_atomically();
        std::cout << "Video globals checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
