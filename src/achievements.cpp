#include "achievements.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

namespace sfr {
namespace fs = std::filesystem;

namespace {
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
constexpr uint32_t tag(const char (&t)[5]) { return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 | uint8_t(t[3]); }

bool fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
}

// An XDBF entry's bytes, by section and id.
struct Xdbf {
    struct Entry { uint16_t section; uint64_t id; std::span<const uint8_t> data; };
    std::vector<Entry> entries;
    std::span<const uint8_t> find(uint16_t section, uint64_t id) const {
        for (const auto& entry : entries)
            if (entry.section == section && entry.id == id) return entry.data;
        return {};
    }
};

bool read_xdbf(std::span<const uint8_t> bytes, Xdbf& out, std::string* error) {
    if (bytes.size() < 24 || be32(bytes.data()) != tag("XDBF")) return fail(error, "not an XDBF resource");
    const uint64_t entry_count = be32(bytes.data() + 8), entry_used = be32(bytes.data() + 12);
    const uint64_t free_count = be32(bytes.data() + 16);
    const uint64_t table_end = 24 + entry_count * 18, data_start = table_end + free_count * 8;
    if (entry_used > entry_count || data_start > bytes.size()) return fail(error, "XDBF tables run past the resource");
    for (uint64_t i = 0; i < entry_used; ++i) {
        const uint8_t* at = bytes.data() + 24 + i * 18;
        const uint64_t offset = be32(at + 10), size = be32(at + 14);
        if (data_start + offset + size > bytes.size()) return fail(error, "an XDBF entry runs past the resource");
        out.entries.push_back({be16(at), be64(at + 2), bytes.subspan(size_t(data_start + offset), size_t(size))});
    }
    return true;
}

// An XSTR table: string id -> UTF-8 text.
std::optional<std::map<uint16_t, std::string>> read_strings(std::span<const uint8_t> table) {
    if (table.size() < 14 || be32(table.data()) != tag("XSTR")) return std::nullopt;
    std::map<uint16_t, std::string> strings;
    const uint32_t count = be16(table.data() + 12);
    size_t at = 14;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 4 > table.size()) return std::nullopt;
        const uint16_t id = be16(table.data() + at), length = be16(table.data() + at + 2);
        if (at + 4 + length > table.size()) return std::nullopt;
        strings[id] = std::string(reinterpret_cast<const char*>(table.data() + at + 4), length);
        at += 4 + size_t(length);
    }
    return strings;
}

// Days since 1970-01-01 of a civil date, and back (Howard Hinnant's algorithms).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

std::string trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return std::string(text);
}
}

std::optional<TitleData> parse_title_data(std::span<const uint8_t> bytes, uint32_t locale, std::string* error) {
    Xdbf xdbf;
    if (!read_xdbf(bytes, xdbf, error)) return std::nullopt;
    const auto table = xdbf.find(1, tag("XACH"));
    if (table.size() < 14 || be32(table.data()) != tag("XACH")) {
        fail(error, "the title data has no achievement table");
        return std::nullopt;
    }
    // The strings: the locale asked for, the title's default, English.
    uint32_t fallback = 1;
    if (const auto stc = xdbf.find(1, tag("XSTC")); stc.size() >= 16 && be32(stc.data()) == tag("XSTC"))
        fallback = be32(stc.data() + 12);
    TitleData title;
    std::optional<std::map<uint16_t, std::string>> strings;
    for (const uint32_t candidate : {locale, fallback, 1u}) {
        if (!candidate) continue;
        if ((strings = read_strings(xdbf.find(3, candidate)))) {
            title.locale = candidate;
            break;
        }
    }
    if (!strings) {
        fail(error, "the title data has no readable string table");
        return std::nullopt;
    }
    const auto text = [&](uint16_t id) {
        const auto found = strings->find(id);
        return found == strings->end() ? std::string() : found->second;
    };
    const uint32_t count = be16(table.data() + 12);
    if (14 + uint64_t(count) * 36 > table.size()) {
        fail(error, "the achievement table runs past its entry");
        return std::nullopt;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* at = table.data() + 14 + i * 36;
        TitleAchievement achievement;
        achievement.id = be16(at);
        achievement.name = text(be16(at + 2));
        achievement.description = text(be16(at + 4));
        achievement.locked_description = text(be16(at + 6));
        achievement.image_id = be32(at + 8);
        achievement.gamerscore = be16(at + 12);
        achievement.flags = be32(at + 16);
        title.achievements.push_back(std::move(achievement));
    }
    for (const auto& entry : xdbf.entries)
        if (entry.section == 2 && entry.id <= 0xFFFFFFFFu &&
            std::any_of(title.achievements.begin(), title.achievements.end(),
                        [&](const TitleAchievement& a) { return a.image_id == entry.id; }))
            title.images[uint32_t(entry.id)].assign(entry.data.begin(), entry.data.end());
    return title;
}

namespace {
struct ResourceRange { std::string name; uint32_t address, size; };
// The XEX header's resources, the one named after the title id first.
std::vector<ResourceRange> title_resource_candidates(std::span<const uint8_t> header, std::string* error) {
    std::vector<ResourceRange> found;
    const auto fits = [&](uint64_t offset, uint64_t size) { return offset <= header.size() && size <= header.size() - offset; };
    if (!fits(0, 24) || be32(header.data()) != tag("XEX2")) {
        fail(error, "xex_header.bin is not an XEX2 header");
        return found;
    }
    const uint32_t count = be32(header.data() + 20);
    if (count > 256 || !fits(24, uint64_t(count) * 8)) {
        fail(error, "the XEX header's table is damaged");
        return found;
    }
    std::string title_name;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t key = be32(header.data() + 24 + i * 8), value = be32(header.data() + 28 + i * 8);
        if (key == 0x00040006 && fits(value, 16)) {  // execution info: media id, version, base version, title id
            char name[9];
            std::snprintf(name, sizeof name, "%08X", be32(header.data() + value + 12));
            title_name = name;
        }
        if (key == 0x000002FF && fits(value, 4)) {
            const uint32_t size = be32(header.data() + value);
            for (uint64_t at = uint64_t(value) + 4; at + 16 <= uint64_t(value) + size && fits(at, 16); at += 16) {
                std::string name;
                for (uint32_t j = 0; j < 8 && header[size_t(at + j)]; ++j) name.push_back(char(header[size_t(at + j)]));
                found.push_back({name, be32(header.data() + at + 8), be32(header.data() + at + 12)});
            }
        }
    }
    std::stable_partition(found.begin(), found.end(), [&](const ResourceRange& r) { return r.name == title_name; });
    if (found.empty()) fail(error, "the game has no title data resource");
    return found;
}
}

std::vector<uint8_t> find_title_resource(std::span<const uint8_t> header, std::span<const uint8_t> image, uint32_t base,
                                         std::string* error) {
    for (const auto& range : title_resource_candidates(header, error)) {
        if (range.address < base || uint64_t(range.address - base) + range.size > image.size() || range.size < 4) continue;
        const auto bytes = image.subspan(range.address - base, range.size);
        if (be32(bytes.data()) == tag("XDBF")) return {bytes.begin(), bytes.end()};
    }
    fail(error, "the game has no title data resource");
    return {};
}

std::vector<uint8_t> read_title_resource(const fs::path& directory, std::string* error) {
    const auto read_all = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::vector<uint8_t> header = read_all(directory / "xex_header.bin");
    if (header.empty()) {
        fail(error, "xex_header.bin is missing");
        return {};
    }
    uint64_t base = 0;
    {
        std::ifstream table(directory / "image.tsv");
        std::string line;
        while (std::getline(table, line))
            if (line.rfind("base\t", 0) == 0) base = std::strtoull(line.c_str() + 5, nullptr, 10);
    }
    if (!base || base > 0xFFFFFFFFu) {
        fail(error, "image.tsv gives no image base");
        return {};
    }
    std::ifstream image(directory / "image.bin", std::ios::binary);
    if (!image) {
        fail(error, "image.bin is missing");
        return {};
    }
    image.seekg(0, std::ios::end);
    const uint64_t image_size = uint64_t(image.tellg());
    for (const auto& range : title_resource_candidates(header, error)) {
        if (range.address < base || range.address - base + uint64_t(range.size) > image_size || range.size < 4) continue;
        std::vector<uint8_t> bytes(range.size);
        image.clear();
        image.seekg(std::streamoff(range.address - base));
        if (!image.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) continue;
        if (be32(bytes.data()) == tag("XDBF")) return bytes;
    }
    fail(error, "the game has no title data resource");
    return {};
}

// ------------------------------------------------ unlocked
const UnlockedAchievement* UnlockedAchievements::find(uint32_t id) const {
    for (const auto& entry : list)
        if (entry.id == id) return &entry;
    return nullptr;
}

fs::path achievement_file(const fs::path& saves, uint64_t xuid) {
    char owner[17];
    std::snprintf(owner, sizeof owner, "%016llX", static_cast<unsigned long long>(xuid));
    return saves / owner / "achievements.txt";
}

fs::path newest_achievement_file(const fs::path& saves) {
    fs::path newest;
    fs::file_time_type newest_time{};
    std::error_code error;
    for (fs::directory_iterator player(saves, error), end; !error && player != end; player.increment(error)) {
        const fs::path file = player->path() / "achievements.txt";
        std::error_code missing;
        const auto time = fs::last_write_time(file, missing);
        if (missing) continue;
        if (newest.empty() || time > newest_time) {
            newest = file;
            newest_time = time;
        }
    }
    return newest;
}

std::string utc_text(int64_t seconds) {
    const int64_t days = (seconds >= 0 ? seconds : seconds - 86399) / 86400, rest = seconds - days * 86400;
    int64_t year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, year, month, day);
    char text[40];
    std::snprintf(text, sizeof text, "%04lld-%02u-%02uT%02d:%02d:%02dZ", static_cast<long long>(year), month, day,
                  int(rest / 3600), int(rest / 60 % 60), int(rest % 60));
    return text;
}

bool parse_utc_text(std::string_view text, int64_t& seconds) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        text[19] != 'Z')
        return false;
    const auto number = [&](size_t at, size_t length, int& out) {
        out = 0;
        for (size_t i = at; i < at + length; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            out = out * 10 + (text[i] - '0');
        }
        return true;
    };
    int year, month, day, hour, minute, second;
    if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day) || !number(11, 2, hour) ||
        !number(14, 2, minute) || !number(17, 2, second))
        return false;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
    seconds = days_from_civil(year, unsigned(month), unsigned(day)) * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

UnlockedAchievements parse_unlocked(std::string_view text) {
    UnlockedAchievements unlocked;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        const std::string clean = trim(line);
        if (clean.empty() || clean[0] == '#') continue;
        const size_t equals = clean.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(std::string_view(clean).substr(0, equals));
        const std::string value = trim(std::string_view(clean).substr(equals + 1));
        if (key == "player") {
            unlocked.player = value;
            continue;
        }
        if (key.empty() || key.size() > 9 || !std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        const uint32_t id = uint32_t(std::strtoul(key.c_str(), nullptr, 10));
        int64_t when = 0;
        if (!parse_utc_text(value, when) || unlocked.find(id)) continue;
        unlocked.list.push_back({id, when});
    }
    return unlocked;
}

std::string format_unlocked(const UnlockedAchievements& unlocked) {
    std::string text =
        "# Free Riders Recompiled: the achievements this player has unlocked, each with\n"
        "# the time it was first awarded (UTC). The launcher's Achievements page shows them.\n";
    if (!unlocked.player.empty()) text += "player=" + unlocked.player + "\n";
    for (const auto& entry : unlocked.list) text += std::to_string(entry.id) + "=" + utc_text(entry.unlocked) + "\n";
    return text;
}

UnlockedAchievements read_unlocked(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parse_unlocked(text.str());
}

bool write_unlocked(const fs::path& file, const UnlockedAchievements& unlocked) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    fs::path temporary = file;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::string text = format_unlocked(unlocked);
        if (!out.write(text.data(), std::streamsize(text.size())) || !out.flush()) return false;
    }
    fs::rename(temporary, file, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

std::vector<uint32_t> add_unlocked(UnlockedAchievements& unlocked, std::span<const uint32_t> ids, int64_t now) {
    std::vector<uint32_t> added;
    for (const uint32_t id : ids) {
        if (unlocked.find(id)) continue;
        unlocked.list.push_back({id, now});
        added.push_back(id);
    }
    return added;
}

// ------------------------------------------------ the game's store
struct AchievementStore::State {
    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::pair<uint32_t, int64_t>> pending;  // awarded, not yet written
    bool busy = false, stopping = false;
    std::string player;
    std::thread worker;
};

AchievementStore::AchievementStore(fs::path file, std::string player) : file_(std::move(file)), state_(std::make_unique<State>()) {
    state_->player = std::move(player);
    State& state = *state_;
    const fs::path path = file_;
    state.worker = std::thread([&state, path] {
        bool loaded = false;
        UnlockedAchievements unlocked;
        std::unique_lock<std::mutex> hold(state.lock);
        for (;;) {
            state.changed.wait(hold, [&] { return state.stopping || !state.pending.empty(); });
            if (state.pending.empty()) return;
            auto batch = std::move(state.pending);
            state.pending.clear();
            state.busy = true;
            const std::string player = state.player;
            hold.unlock();
            if (!loaded) {
                unlocked = read_unlocked(path);
                loaded = true;
            }
            if (unlocked.player.empty()) unlocked.player = player;
            bool added = false;
            for (const auto& [id, when] : batch) {
                const uint32_t one[] = {id};
                added |= !add_unlocked(unlocked, one, when).empty();
            }
            if (added && !write_unlocked(path, unlocked))
                std::fprintf(stderr, "ACHIEVEMENTS write failed path=%s\n", reinterpret_cast<const char*>(path.u8string().c_str()));
            hold.lock();
            state.busy = false;
            state.changed.notify_all();
        }
    });
}

AchievementStore::~AchievementStore() {
    {
        std::lock_guard<std::mutex> hold(state_->lock);
        state_->stopping = true;
    }
    state_->changed.notify_all();
    if (state_->worker.joinable()) state_->worker.join();
}

void AchievementStore::award(std::span<const uint32_t> ids, int64_t now) {
    {
        std::lock_guard<std::mutex> hold(state_->lock);
        for (const uint32_t id : ids) state_->pending.emplace_back(id, now);
    }
    state_->changed.notify_all();
}

void AchievementStore::wait() {
    std::unique_lock<std::mutex> hold(state_->lock);
    state_->changed.wait(hold, [&] { return state_->pending.empty() && !state_->busy; });
}
}
