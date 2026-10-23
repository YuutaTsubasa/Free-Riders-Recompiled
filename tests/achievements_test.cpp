// The achievements' title data parser (on a small XDBF made here: no game
// data is in the repository) and the unlocked list's file.
#include "achievements.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool ok, const char* detail) {
    if (!ok) throw std::runtime_error(detail);
}

void put16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}
void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
}
void put_tag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

std::vector<uint8_t> string_table(const std::vector<std::pair<uint16_t, std::string>>& strings) {
    std::vector<uint8_t> out;
    put_tag(out, "XSTR");
    put32(out, 1);
    put32(out, 0);  // size, which nothing reads
    put16(out, uint32_t(strings.size()));
    for (const auto& [id, text] : strings) {
        put16(out, id);
        put16(out, uint32_t(text.size()));
        out.insert(out.end(), text.begin(), text.end());
    }
    return out;
}

void achievement(std::vector<uint8_t>& out, uint16_t id, uint16_t name, uint16_t description, uint16_t locked,
                 uint32_t image, uint16_t gamerscore, uint32_t flags) {
    put16(out, id);
    put16(out, name);
    put16(out, description);
    put16(out, locked);
    put32(out, image);
    put16(out, gamerscore);
    put16(out, 0);
    put32(out, flags);
    for (int i = 0; i < 4; ++i) put32(out, 0);
}

// An XDBF as a title's SPA lays it out: entries (section, id, offset, size),
// a free list, then the data.
std::vector<uint8_t> title_blob() {
    struct Entry { uint16_t section; uint64_t id; std::vector<uint8_t> data; };
    std::vector<Entry> entries;
    std::vector<uint8_t> xach;
    put_tag(xach, "XACH");
    put32(xach, 1);
    put32(xach, 0);
    put16(xach, 3);
    achievement(xach, 6, 1, 2, 3, 10, 10, 0x0E);   // shown while locked
    achievement(xach, 11, 4, 5, 6, 11, 15, 0x0C);
    achievement(xach, 30, 7, 8, 9, 12, 40, 0x03);  // secret
    entries.push_back({1, 0x58414348, xach});
    std::vector<uint8_t> xstc;
    put_tag(xstc, "XSTC");
    put32(xstc, 1);
    put32(xstc, 4);
    put32(xstc, 1);  // English by default
    entries.push_back({1, 0x58535443, xstc});
    entries.push_back({3, 1, string_table({{1, "Welcome"}, {2, "You cleared it!"}, {3, "Clear it."},
                                           {4, "First"}, {5, "You won!"}, {6, "Win a race."},
                                           {7, "Collector"}, {8, "All of them!"}, {9, "Collect all."}})});
    entries.push_back({3, 3, string_table({{1, "Willkommen"}, {2, "Geschafft!"}, {3, "Schaffe es."},
                                           {4, "Erster"}, {5, "Gewonnen!"}, {6, "Gewinne."},
                                           {7, "Sammler"}, {8, "Alle!"}, {9, "Sammle alle."}})});
    entries.push_back({2, 10, {0x89, 'P', 'N', 'G', 1}});
    entries.push_back({2, 11, {0x89, 'P', 'N', 'G', 2}});
    entries.push_back({2, 12, {0x89, 'P', 'N', 'G', 3}});
    entries.push_back({2, 99, {0x89, 'P', 'N', 'G', 9}});  // not an achievement's

    const uint32_t entry_count = uint32_t(entries.size()) + 2, free_count = 1;  // room to spare, as real ones have
    std::vector<uint8_t> out, data;
    put_tag(out, "XDBF");
    put32(out, 0x10000);
    put32(out, entry_count);
    put32(out, uint32_t(entries.size()));
    put32(out, free_count);
    put32(out, 1);
    for (const auto& entry : entries) {
        put16(out, entry.section);
        put32(out, uint32_t(entry.id >> 32));
        put32(out, uint32_t(entry.id));
        put32(out, uint32_t(data.size()));
        put32(out, uint32_t(entry.data.size()));
        data.insert(data.end(), entry.data.begin(), entry.data.end());
    }
    out.resize(out.size() + (entry_count - entries.size()) * 18 + free_count * 8, 0);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

void parses_title_data() {
    const auto blob = title_blob();
    const auto english = sfr::parse_title_data(blob, 1);
    require(english && english->locale == 1, "English strings");
    require(english->achievements.size() == 3, "three achievements");
    const auto& first = english->achievements[0];
    require(first.id == 6 && first.name == "Welcome" && first.description == "You cleared it!" &&
                first.locked_description == "Clear it." && first.image_id == 10 && first.gamerscore == 10,
            "first achievement's fields");
    require(!first.secret() && english->achievements[2].secret(), "secret flag");
    require(english->images.size() == 3 && english->images.at(11).back() == 2 && !english->images.count(99),
            "the achievements' images only");

    const auto german = sfr::parse_title_data(blob, 3);
    require(german && german->locale == 3 && german->achievements[1].name == "Erster", "German strings");
    // Japanese is not in this blob: the default locale (English).
    const auto japanese = sfr::parse_title_data(blob, 2);
    require(japanese && japanese->locale == 1 && japanese->achievements[0].name == "Welcome", "fallback to default");

    std::string error;
    require(!sfr::parse_title_data(std::span<const uint8_t>(blob.data(), 40), 1, &error) && !error.empty(),
            "truncated blob refused");
    auto bad = blob;
    bad[0] = 'Y';
    require(!sfr::parse_title_data(bad, 1), "wrong magic refused");
}

// A raw XEX2 header with an execution info (title 5345084D) and a resource
// table naming another resource first and the title's second.
std::vector<uint8_t> xex_header(uint32_t title_address, uint32_t title_size) {
    std::vector<uint8_t> out;
    put_tag(out, "XEX2");
    for (int i = 0; i < 4; ++i) put32(out, 0);
    put32(out, 2);  // two optional headers
    const uint32_t execution = 24 + 16, resources = execution + 24;
    put32(out, 0x00040006);
    put32(out, execution);
    put32(out, 0x000002FF);
    put32(out, resources);
    put32(out, 0x01020304);  // media id
    put32(out, 1);
    put32(out, 1);
    put32(out, 0x5345084D);  // title id
    put32(out, 0);
    put32(out, 0);
    put32(out, 4 + 16 * 2);
    for (const char* name : {"TEX_00\0\0", "5345084D"}) {
        out.insert(out.end(), name, name + 8);
        const bool title = name[0] == '5';
        put32(out, title ? title_address : 0x82000000);
        put32(out, title ? title_size : 16);
    }
    return out;
}

void finds_title_resource(const fs::path& directory) {
    const auto blob = title_blob();
    constexpr uint32_t base = 0x82000000;
    const uint32_t at = 0x100;
    std::vector<uint8_t> image(at, 0);
    image.insert(image.end(), blob.begin(), blob.end());
    const auto header = xex_header(base + at, uint32_t(blob.size()));
    require(sfr::find_title_resource(header, image, base) == blob, "resource found in memory");

    fs::create_directories(directory);
    std::ofstream(directory / "xex_header.bin", std::ios::binary).write(reinterpret_cast<const char*>(header.data()), header.size());
    std::ofstream(directory / "image.bin", std::ios::binary).write(reinterpret_cast<const char*>(image.data()), image.size());
    std::ofstream(directory / "image.tsv") << "base\t" << base << "\nsize\t" << image.size() << "\n";
    std::string error;
    require(sfr::read_title_resource(directory, &error) == blob, "resource read from an image directory");
    require(sfr::read_title_resource(directory / "missing", &error).empty() && !error.empty(), "missing directory");
}

void unlocked_round_trip(const fs::path& saves) {
    require(sfr::utc_text(0) == "1970-01-01T00:00:00Z", "epoch text");
    require(sfr::utc_text(1700000000) == "2023-11-14T22:13:20Z", "known time text");
    int64_t seconds = 0;
    require(sfr::parse_utc_text("2023-11-14T22:13:20Z", seconds) && seconds == 1700000000, "known time parsed");
    require(sfr::parse_utc_text("2000-02-29T12:00:00Z", seconds) && sfr::utc_text(seconds) == "2000-02-29T12:00:00Z", "leap day");
    require(!sfr::parse_utc_text("2023-11-14 22:13:20", seconds), "other formats refused");

    const fs::path file = sfr::achievement_file(saves, 0xE000123456789ABCull);
    require(file == saves / "E000123456789ABC" / "achievements.txt", "file beside the player's saves");

    sfr::UnlockedAchievements unlocked;
    unlocked.player = "Player";
    const uint32_t first[] = {11, 6, 11};
    require(sfr::add_unlocked(unlocked, first, 1700000000).size() == 2, "duplicates added once");
    const uint32_t again[] = {6, 30};
    const auto added = sfr::add_unlocked(unlocked, again, 1700000100);
    require(added.size() == 1 && added[0] == 30 && unlocked.find(6)->unlocked == 1700000000, "first time kept");
    require(sfr::write_unlocked(file, unlocked), "written");
    const auto read = sfr::read_unlocked(file);
    require(read.player == "Player" && read.list.size() == 3 && read.list[0].id == 11 && read.list[2].unlocked == 1700000100,
            "read back");
    require(!fs::exists(fs::path(file).concat(".new")), "no temporary file left");
    require(sfr::newest_achievement_file(saves) == file, "found among the players");

    const auto parsed = sfr::parse_unlocked("# note\r\nplayer = Rider \r\n12=2023-11-14T22:13:20Z\r\nbad\r\nx=1\r\n13=yesterday\r\n");
    require(parsed.player == "Rider" && parsed.list.size() == 1 && parsed.list[0].id == 12, "lenient parse");
}

void store_writes_off_thread(const fs::path& saves) {
    const fs::path file = sfr::achievement_file(saves, 0xE000000000000001ull);
    {
        sfr::AchievementStore store(file, "Solo");
        const uint32_t race[] = {11};
        store.award(race, 1700000000);
        store.wait();
        require(sfr::read_unlocked(file).list.size() == 1, "first award written");
        const uint32_t more[] = {11, 6};
        store.award(more, 1700000500);
        store.wait();
    }
    {
        // A later run keeps what is there.
        sfr::AchievementStore store(file, "Solo");
        const uint32_t again[] = {6, 12};
        store.award(again, 1700001000);
    }  // the destructor writes what is pending
    const auto read = sfr::read_unlocked(file);
    require(read.player == "Solo" && read.list.size() == 3, "three unlocked");
    require(read.find(11)->unlocked == 1700000000 && read.find(6)->unlocked == 1700000500 &&
                read.find(12)->unlocked == 1700001000,
            "each kept its first time");
}
}

int main() {
    try {
        const fs::path root = fs::temp_directory_path() / ("sfr-achievements-test-" + std::to_string(sfr::unix_now()));
        std::error_code ignored;
        fs::remove_all(root, ignored);
        parses_title_data();
        finds_title_resource(root / "image");
        unlocked_round_trip(root / "save-a");
        store_writes_off_thread(root / "save-b");
        fs::remove_all(root, ignored);
        std::cout << "achievements: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "achievements: " << error.what() << '\n';
        return 1;
    }
}
