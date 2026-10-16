#pragma once
#include <cstdint>
#include <utility>
#include <vector>

// SFR_TEST_DRAW_READS (draw_read_watch.cpp): who reads, between two race
// frames' drawing, the words the drawing wrote.
namespace sfr::draw_reads {
struct Word { uint32_t address, vtable, offset; };
bool enabled();
// Watches the words until end(); a read of one is recorded with its owner.
void begin(uint8_t* base, std::vector<Word> written, uint32_t (*current_thread)());
void end();
// Prints DRAW_READS: each reading instruction (rip, a link-map offset) with
// the owner (vtable+offset) and count since the last report.
void report(uint32_t frame);
// Pages the main thread writes between begin_capture and end_capture, each
// with its contents before the first write (valid until the next capture).
struct CapturedPage { uint32_t address; const uint8_t* before; };
void begin_capture(uint8_t* base, std::vector<std::pair<uint64_t, uint64_t>> ranges, uint32_t (*current_thread)());
std::vector<CapturedPage> end_capture();
}
