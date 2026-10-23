#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {
// ------------------------------------------------ what the game declares
// The title's achievements as its XEX carries them: an XDBF ("SPA") resource
// named after the title id, with an XACH table (id, name/description string
// ids, image id, gamerscore, flags), the strings per locale (XSTR, section 3,
// id = Xbox language: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish,
// 6 Italian), the default locale (XSTC) and the icons as PNG (section 2).
// Layouts as Xenia reads them (kernel/util/xdbf_utils.h).
struct TitleAchievement {
    uint16_t id = 0;
    std::string name;
    std::string description;         // shown once unlocked
    std::string locked_description;  // how to unlock it
    uint32_t image_id = 0;
    uint16_t gamerscore = 0;
    uint32_t flags = 0;
    // Bit 3 says its text may be shown while locked; without it the
    // achievement is secret until unlocked.
    bool secret() const { return (flags & 0x8u) == 0; }
};
struct TitleData {
    uint32_t locale = 0;  // the string table the texts came from
    std::vector<TitleAchievement> achievements;
    std::map<uint32_t, std::vector<uint8_t>> images;  // image id -> PNG bytes
};
// The XDBF bytes, with the texts of `locale`: its table, else the default
// locale's (XSTC), else English. Empty (error says why) if malformed.
std::optional<TitleData> parse_title_data(std::span<const uint8_t> xdbf, uint32_t locale, std::string* error = nullptr);

// The XDBF resource of a runtime image directory (xex_header.bin's resource
// table and execution info, image.bin, image.tsv's base): the resource named
// after the title id. Empty (error says why) when it is missing.
std::vector<uint8_t> read_title_resource(const std::filesystem::path& image_directory, std::string* error = nullptr);
// The same, found in a raw XEX header and the image it loads at base.
std::vector<uint8_t> find_title_resource(std::span<const uint8_t> xex_header, std::span<const uint8_t> image,
                                         uint32_t base, std::string* error = nullptr);

// ------------------------------------------------ what a player unlocked
// Kept per profile beside its saves: <saves>/<XUID>/achievements.txt, a text
// file the game adds to (never rewrites an unlock's time) and the launcher reads.
//   # comment
//   player=<gamertag>
//   <id>=<YYYY-MM-DDTHH:MM:SSZ>   first awarded, UTC
struct UnlockedAchievement {
    uint32_t id = 0;
    int64_t unlocked = 0;  // seconds since 1970, UTC
};
struct UnlockedAchievements {
    std::string player;
    std::vector<UnlockedAchievement> list;  // in the order they were awarded
    const UnlockedAchievement* find(uint32_t id) const;
};
std::filesystem::path achievement_file(const std::filesystem::path& saves, uint64_t xuid);
// The newest achievements file among the players in a saves folder.
std::filesystem::path newest_achievement_file(const std::filesystem::path& saves);
UnlockedAchievements parse_unlocked(std::string_view text);
std::string format_unlocked(const UnlockedAchievements& unlocked);
UnlockedAchievements read_unlocked(const std::filesystem::path& file);
// Through a temporary file renamed over the old one, so a stop half way
// through leaves the old list.
bool write_unlocked(const std::filesystem::path& file, const UnlockedAchievements& unlocked);
// Adds the ids not in the list yet, unlocked at `now`. Returns those added.
std::vector<uint32_t> add_unlocked(UnlockedAchievements& unlocked, std::span<const uint32_t> ids, int64_t now);

// "2026-10-24T05:12:33Z" and back (false for anything else).
std::string utc_text(int64_t seconds);
bool parse_utc_text(std::string_view text, int64_t& seconds);
int64_t unix_now();

// The game's side: awards go to a thread of their own, which reads the file
// once and writes it again when something new arrived, so the game's thread
// never waits for the disk.
class AchievementStore {
public:
    AchievementStore(std::filesystem::path file, std::string player);
    ~AchievementStore();
    AchievementStore(const AchievementStore&) = delete;
    AchievementStore& operator=(const AchievementStore&) = delete;
    void award(std::span<const uint32_t> ids, int64_t now);
    // Waits until everything awarded so far is written (tests, shutdown).
    void wait();
    const std::filesystem::path& file() const { return file_; }
private:
    struct State;
    std::filesystem::path file_;
    std::unique_ptr<State> state_;
};
}
