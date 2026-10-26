#include "achievement_progress.h"
#include <algorithm>
#include <fstream>
#include <system_error>

namespace sfr {
namespace {
using Kind = AchievementCounter::Kind;
constexpr uint64_t bit(unsigned index) { return uint64_t(1) << index; }
// Bytes 11, 16 and 39..45 of the gear set are passed over (8223150C).
constexpr uint64_t gear_skips = bit(11) | bit(16) | bit(39) | bit(40) | bit(41) | bit(42) | bit(43) | bit(44) | bit(45);

// Where each was found (the game's code that adds to it or checks it).
constexpr AchievementCounter counters[] = {
    // 82231AD0: one more 1st place in an offline Normal Race; 16 or more.
    {13, Kind::count, 0x1954, 0, 16},
    // 822348B4 adds a race's Rings (to at most 99,999); 822318C8: 10,000.
    {14, Kind::count, 0x001C, 0, 10000},
    // 82231310, events 15..22, 53, 54, 56: one more each time; 30.
    {15, Kind::count, 0x19EC, 0, 30},  // Bomb-Happy: Target Torpedo hits
    {16, Kind::count, 0x19F0, 0, 30},  // Rocket Star: Soda Rockets
    {17, Kind::count, 0x19F4, 0, 30},  // Punchout: Bowling Strike hits
    {18, Kind::count, 0x19F8, 0, 30},  // Masters: Tee Shot hits
    {19, Kind::count, 0x19FC, 0, 30},  // Troublemaker: Octo-Ink hits
    {20, Kind::count, 0x1A00, 0, 30},  // Clutch Shooter: Free Throws
    {21, Kind::count, 0x1A04, 0, 30},  // Ringmaker: Ring Cans
    {22, Kind::count, 0x1A08, 0, 30},  // Boost Star: Octo-Boosts
    {53, Kind::count, 0x1A1C, 0, 30},  // Charred King: hit by Target Torpedoes
    {54, Kind::count, 0x1A20, 0, 30},  // Gutter King: hit by Bowling Strikes
    {56, Kind::count, 0x1A34, 0, 30},  // Jolt King: shocked at the start line
    // Course sets: 8 courses x standard/expert, set after a race of the mode
    // (82231A10 Tag, 82231A54 Relay, 82231B10 Time Attack, 82231FE0 Ring
    // Collection, 82232040 Damage Survival).
    {24, Kind::flags, 0x1958, 16, 0},
    {25, Kind::flags, 0x1968, 16, 0},
    {27, Kind::flags, 0x1978, 16, 0},
    {28, Kind::flags, 0x1988, 16, 0},
    {29, Kind::flags, 0x1998, 16, 0},
    {26, Kind::flags, 0x09A8, 27, 0},  // 822319BC: tutorial topics
    {30, Kind::flags, 0x09D8, 46, 0, gear_skips},  // 8223150C: gears owned
    {31, Kind::flags, 0x0A62, 21, 0},  // 8223158C: gear parts owned
    {49, Kind::flags, 0x1A0C, 8, 0},   // 822315DC: X-rank trick, standard courses
    {50, Kind::flags, 0x1A14, 8, 0},   // 82231624: X-rank trick, expert courses
    {51, Kind::flags, 0x19A8, 19, 0},  // 822320D0: 1st with each character
    {52, Kind::flags, 0x19BB, 46, 0},  // 82232124: 1st with each gear
    {55, Kind::flags, 0x1A24, 16, 0},  // 822316AC: UNBELIEVABLE start dash per course
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
}

std::span<const AchievementCounter> achievement_counters() { return counters; }

std::vector<AchievementProgress> read_achievement_progress(std::span<const uint8_t> record) {
    std::vector<AchievementProgress> list;
    if (record.size() < achievement_record_size) return list;
    for (const auto& counter : counters) {
        if (counter.kind == Kind::count) {
            list.push_back({counter.id, (std::min)(be32(record.data() + counter.offset), counter.target), counter.target});
            continue;
        }
        uint32_t set = 0, total = 0;
        for (uint32_t i = 0; i < counter.length; ++i) {
            if (i < 64 && (counter.skipped & bit(i))) continue;
            ++total;
            if (record[counter.offset + i]) ++set;
        }
        list.push_back({counter.id, set, total});
    }
    return list;
}

const AchievementProgress* find_progress(std::span<const AchievementProgress> list, uint16_t id) {
    for (const auto& progress : list)
        if (progress.id == id) return &progress;
    return nullptr;
}

std::optional<uint32_t> achievement_record_address(const std::function<std::optional<uint32_t>(uint32_t)>& load32) {
    const auto table = load32(player_table_pointer);
    if (!table || *table < 0x40000000u) return std::nullopt;
    const auto index_word = load32(*table + player_index_offset);
    if (!index_word) return std::nullopt;
    const int8_t index = int8_t(*index_word & 0xFF);
    if (index < 0 || index > 3) return std::nullopt;
    return *table + uint32_t(index) * player_entry_size + player_record_offset;
}

std::vector<AchievementProgress> read_save_progress(std::span<const uint8_t> save) {
    if (save.size() < achievement_record_size - save_record_skip) return {};
    std::vector<uint8_t> record(achievement_record_size);
    std::copy(save.begin(), save.begin() + (achievement_record_size - save_record_skip), record.begin() + save_record_skip);
    return read_achievement_progress(record);
}

std::filesystem::path player_save_file(const std::filesystem::path& player_directory) {
    const auto content = player_directory / "00000001" / "SfrAllData.sav";
    std::error_code error;
    // The content's files are kept in a folder named after the content.
    if (std::filesystem::is_directory(content, error)) return content / "SfrAllData.sav";
    return content;
}

std::vector<AchievementProgress> read_player_progress(const std::filesystem::path& player_directory) {
    std::ifstream in(player_save_file(player_directory), std::ios::binary);
    std::vector<uint8_t> bytes(achievement_record_size - save_record_skip);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return {};
    return read_save_progress(bytes);
}
}
