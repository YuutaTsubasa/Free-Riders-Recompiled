#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// SFR_TEST_DRAW_READS (draw_read_watch.cpp): who reads, between two race
// frames' drawing, the words the drawing wrote.
namespace sfr::draw_reads {
// owner: the task whose drawing wrote the word (0 if not known).
struct Word { uint32_t address, vtable, offset, owner = 0; };
bool enabled();
// Watches the words until end(); a read of one is recorded with its owner.
void begin(uint8_t* base, std::vector<Word> written, uint32_t (*current_thread)());
void end();
// Ends the watch and returns the words nobody has overwritten since it began.
std::vector<Word> close();
// What the main thread is running, recorded with each read: the task kind
// whose method runs (0 outside the task manager) and the phase's key (0
// outside it), so a read by another kind's drawing can be told from the
// race's logic.
void set_reader(uint32_t kind, uint32_t key);
// Prints DRAW_READS: each reading instruction (rip, a link-map offset) with
// the owner (vtable+offset) and count since the last report.
void report(uint32_t frame);
// Pages the main thread writes between begin_capture and end_capture, each
// with its contents before the first write (valid until the next capture).
struct CapturedPage { uint32_t address; const uint8_t* before; };
void begin_capture(uint8_t* base, std::vector<std::pair<uint64_t, uint64_t>> ranges, uint32_t (*current_thread)());
std::vector<CapturedPage> end_capture();
// Hands over each page written since begin_capture or the last rearm (its
// contents before, and now in memory) and makes it read-only again, so the
// next writes are told apart: what each task's drawing wrote.
void rearm(const std::function<void(const CapturedPage&)>& written);
// Physical memory (from 0xA0000000; its plain read-write pages) is compared
// instead: a copy now, then each word that differs from it.
void snapshot_physical(uint8_t* base, const std::vector<std::pair<uint64_t, uint64_t>>& ranges);
void diff_physical(uint8_t* base, const std::function<void(uint32_t)>& changed);
}
