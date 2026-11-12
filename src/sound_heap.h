#pragma once
#include <cstdint>
#include <functional>

namespace sfr {
// The sound library's heap (Issue #385). At start the title makes one heap for
// all of its sound banks (8222EA60 calls the heap constructor 827E2450 with a
// buffer and its size, and keeps the heap at 83E50B40): 18,979,226 bytes of
// the executable's own data at 82C32960, a fixed size on the console too.
// The banks of a race (the course's effects and every racer's voices, plus
// Super Sonic's SRN_Eact_SP.csb when Sonic rides the Chaos Emeralds,
// 8222EF10) are loaded into it; a bank that does not fit fails in 827B8800
// (E2007030208A, -3: no memory) and the race waits for it for ever on the
// "Now Loading" screen. The original banks leave room, but voice mods with
// larger files (GameBanana's Zero Gravity Voice Pack: 7.9 MB of voices
// against 5.8 MB) do not once Super Sonic's bank joins them.
//
// So the heap is made in a larger block of guest memory instead, when one can
// be had; the executable's buffer is then left unused. Nothing else refers to
// the buffer (the heap is reached through 83E50B40), and the sound library
// asks the heap for every block, so it does not notice where the heap is.
constexpr uint32_t sound_heap_console_buffer = 0x82C32960;
constexpr uint32_t sound_heap_console_size = 0x0121999A;
// 32 MiB less 64 KiB: room for about 13 MB more banks than the console's,
// and still within one 32 MiB block of the host heap (o1heap rounds a block
// and its header up to a power of two).
constexpr uint32_t sound_heap_size = 0x01FF0000;
// The heap handle the host heap files the block under: no title heap has it,
// so destroying a title heap never frees the sound heap.
constexpr uint32_t sound_heap_tag = 0x534E4448;  // "SNDH"

struct SoundHeapRegion {
    uint32_t buffer, size;
    bool moved;
};
// Where the heap constructor should make the heap it was asked to make in
// (buffer, size): the console's sound heap goes to a block of sound_heap_size
// from allocate (a guest address, 0 when there is none), any other heap and a
// failed allocation stay where they were asked for.
SoundHeapRegion sound_heap_region(uint32_t buffer, uint32_t size, const std::function<uint32_t(uint32_t)>& allocate);
}
