// The sound library's heap moved to a larger block (Issue #385).
#include "sound_heap.h"
#include <cstdlib>
#include <iostream>
#include <iterator>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL " << what << '\n';
        ++failures;
    }
}
}

int main() {
    using namespace sfr;
    // The console's heap goes to the block the host heap gives, at the larger size.
    uint32_t asked = 0;
    auto region = sound_heap_region(sound_heap_console_buffer, sound_heap_console_size, [&](uint32_t size) {
        asked = size;
        return 0x50010000u;
    });
    check(region.moved && region.buffer == 0x50010000u && region.size == sound_heap_size, "console heap moved");
    check(asked == sound_heap_size, "the larger size is asked for");
    check(sound_heap_size > sound_heap_console_size + 0x200000, "room for the voice pack's 2.1 MB more");
    // A Chaos Emeralds race's voice banks (Sonic, Super Sonic and the eleven others, in
    // 8222EF10's order), 8-byte aligned as 827B8800 asks: the Zero Gravity Voice Pack's
    // need more than the console's, which filled its heap, and the added room holds the rest.
    const uint32_t original[] = {383616, 433288, 460496, 480080, 469272, 493240, 436424,
                                 465688, 428104, 454360, 506576, 469496, 296048};
    const uint32_t pack[] = {560272, 545360, 681696, 651232, 626736, 575120, 804816,
                             590152, 642408, 712152, 633504, 587808, 296048};
    uint64_t more = 0;
    for (size_t i = 0; i < std::size(pack); ++i) more += ((pack[i] + 7) & ~7u) - ((original[i] + 7) & ~7u);
    check(more > 2000000 && more < sound_heap_size - sound_heap_console_size, "the pack's larger voices fit in the added room");

    // No block: the heap stays where the title put it.
    region = sound_heap_region(sound_heap_console_buffer, sound_heap_console_size, [](uint32_t) { return 0u; });
    check(!region.moved && region.buffer == sound_heap_console_buffer && region.size == sound_heap_console_size,
          "failed allocation keeps the console heap");
    region = sound_heap_region(sound_heap_console_buffer, sound_heap_console_size, nullptr);
    check(!region.moved && region.buffer == sound_heap_console_buffer, "no allocator keeps the console heap");

    // Any other heap the constructor makes is left alone, without allocating.
    bool called = false;
    region = sound_heap_region(0x40001000u, 0x10000u, [&](uint32_t) {
        called = true;
        return 0x50010000u;
    });
    check(!called && !region.moved && region.buffer == 0x40001000u && region.size == 0x10000u, "other heap untouched");
    region = sound_heap_region(sound_heap_console_buffer, 0x10000u, [&](uint32_t) {
        called = true;
        return 0x50010000u;
    });
    check(!called && !region.moved && region.size == 0x10000u, "other size untouched");

    if (failures) return EXIT_FAILURE;
    std::cout << "sound_heap_test: ok\n";
    return EXIT_SUCCESS;
}
