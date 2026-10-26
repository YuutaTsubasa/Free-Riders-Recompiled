#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace sfr {
// How far the player is towards the achievements that need a number of
// things, read from the game's own player record (Issue #288). Nothing here
// is counted by us: the game keeps these counts and flags itself, and awards
// the achievement when its own check passes (82231310 for the counters it
// adds to as things happen, 82231870 for the checks after a race).
//
// The record ("S" below) is the signed-in player's: the player table's
// pointer at 0x83E52F6C (G), the player's index the low byte of G+0x7F08
// (-1: nobody), each entry 0x1FB0 bytes from G+0x10, and S 0x20 into the
// entry. The achievement code reaches it the same way (82231740: the
// entry is G + index * 0x1FB0 + 0x10, its achievement object at +0x1B60,
// whose word +4 points back at the entry; 82231310 adds 0x20 to that).
//
// Counters are big-endian words the game adds one to; flag sets are bytes
// (one per course, character, gear ...) the game sets to 1, and the
// achievement comes when every byte of the set is set.
struct AchievementCounter {
    enum class Kind : uint8_t { count, flags };
    uint16_t id;          // the achievement (XACH id)
    Kind kind;
    uint32_t offset;      // into S
    uint32_t length;      // flags: bytes in the set
    uint32_t target;      // count: the number the game checks for
    uint64_t skipped = 0; // flags: bits of bytes the check passes over
};
std::span<const AchievementCounter> achievement_counters();

// The bytes of S the counters lie in.
constexpr uint32_t achievement_record_size = 0x1A40;

struct AchievementProgress {
    uint16_t id;
    uint32_t value;   // never more than target
    uint32_t target;
};
// Progress from a copy of S's first achievement_record_size bytes (big
// endian, as in guest memory). Shorter copies give nothing.
std::vector<AchievementProgress> read_achievement_progress(std::span<const uint8_t> record);
const AchievementProgress* find_progress(std::span<const AchievementProgress> list, uint16_t id);

// The game's save keeps S as it is, from its byte 0x10 on: SfrAllData.sav
// starts with S + 0x10 (S + 0x10 .. + 0x1A40 matched the save byte for byte
// but for a few header words at S + 0x14 .. + 0x1B; none is a counter).
// The launcher reads progress from there.
constexpr uint32_t save_record_skip = 0x10;
std::vector<AchievementProgress> read_save_progress(std::span<const uint8_t> save);
// The save of a player's folder (<saves>/<XUID>): its content 00000001's
// SfrAllData.sav, as the game's content files keep it (content_files.h).
std::filesystem::path player_save_file(const std::filesystem::path& player_directory);
// Reads only the bytes it needs; empty if the file is missing or short.
std::vector<AchievementProgress> read_player_progress(const std::filesystem::path& player_directory);

// S's guest address through the game's pointers, read with load32 (empty
// when a read fails or nobody is signed in).
constexpr uint32_t player_table_pointer = 0x83E52F6C;
constexpr uint32_t player_index_offset = 0x7F08;
constexpr uint32_t player_entry_size = 0x1FB0;
constexpr uint32_t player_record_offset = 0x30;  // 0x10 to the first entry, 0x20 into it
std::optional<uint32_t> achievement_record_address(const std::function<std::optional<uint32_t>(uint32_t)>& load32);
}
