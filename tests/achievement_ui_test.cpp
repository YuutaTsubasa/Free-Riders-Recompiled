// The achievement notification and page (Issue #288): progress read from the
// player record, the notification's timing, the page's navigation and order,
// and the software drawing.
#include "achievement_overlay.h"
#include "achievement_progress.h"
#include "achievement_toast.h"
#include "image_decode.h"
#include "overlay_canvas.h"
#include "screen_overlay.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) check((x), #x, __LINE__)

void put32(std::vector<uint8_t>& bytes, uint32_t at, uint32_t value) {
    bytes[at] = uint8_t(value >> 24);
    bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8);
    bytes[at + 3] = uint8_t(value);
}

void progress_from_record() {
    using namespace sfr;
    CHECK(read_achievement_progress(std::vector<uint8_t>(achievement_record_size - 1)).empty());
    std::vector<uint8_t> record(achievement_record_size);
    put32(record, 0x19EC, 12);      // Bomb-Happy: 12 Target Torpedo hits
    put32(record, 0x001C, 123456);  // more Rings than Rich King asks
    put32(record, 0x1954, 3);       // three offline wins
    put32(record, 0x1A34, 31);      // Jolt King past its 30
    for (int i : {0, 5, 15}) record[0x1958 + i] = 1;  // Tag: three courses
    record[0x09D8 + 11] = 1;        // a gear the check passes over
    record[0x09D8 + 0] = 1;
    const auto list = read_achievement_progress(record);
    CHECK(list.size() == achievement_counters().size());
    const auto* bomb = find_progress(list, 15);
    CHECK(bomb && bomb->value == 12 && bomb->target == 30);
    const auto* rings = find_progress(list, 14);
    CHECK(rings && rings->value == 10000 && rings->target == 10000);
    const auto* wins = find_progress(list, 13);
    CHECK(wins && wins->value == 3 && wins->target == 16);
    const auto* jolt = find_progress(list, 56);
    CHECK(jolt && jolt->value == 30);
    const auto* tag = find_progress(list, 24);
    CHECK(tag && tag->value == 3 && tag->target == 16);
    const auto* gears = find_progress(list, 30);
    CHECK(gears && gears->value == 1 && gears->target == 37);
    const auto* parts = find_progress(list, 31);
    CHECK(parts && parts->value == 0 && parts->target == 21);
    CHECK(find_progress(list, 6) == nullptr);  // Welcome License counts nothing
    // The save holds the record from its byte 0x10 on.
    std::vector<uint8_t> save(achievement_record_size - save_record_skip + 100);
    put32(save, 0x19EC - save_record_skip, 7);
    put32(save, 0x001C - save_record_skip, 4321);
    const auto saved = read_save_progress(save);
    CHECK(find_progress(saved, 15) && find_progress(saved, 15)->value == 7);
    CHECK(find_progress(saved, 14) && find_progress(saved, 14)->value == 4321);
    CHECK(read_save_progress(std::vector<uint8_t>(100)).empty());
    {
        namespace fs = std::filesystem;
        const fs::path player = fs::temp_directory_path() / "sfr_achievement_ui_test" / "E000000000000001";
        fs::remove_all(player.parent_path());
        CHECK(read_player_progress(player).empty());
        fs::create_directories(player / "00000001" / "SfrAllData.sav");
        CHECK(player_save_file(player) == player / "00000001" / "SfrAllData.sav" / "SfrAllData.sav");
        std::ofstream(player_save_file(player), std::ios::binary).write(reinterpret_cast<const char*>(save.data()), std::streamsize(save.size()));
        const auto from_file = read_player_progress(player);
        CHECK(find_progress(from_file, 15) && find_progress(from_file, 15)->value == 7);
        fs::remove_all(player.parent_path());
    }
    // Every counter lies inside the record.
    for (const auto& c : achievement_counters())
        CHECK(c.offset + (c.kind == AchievementCounter::Kind::count ? 4 : c.length) <= achievement_record_size);
}

void record_address() {
    using namespace sfr;
    std::map<uint32_t, uint32_t> words;
    const auto load = [&](uint32_t address) -> std::optional<uint32_t> {
        const auto found = words.find(address);
        if (found == words.end()) return std::nullopt;
        return found->second;
    };
    CHECK(!achievement_record_address(load));
    words[player_table_pointer] = 0x40100000;
    words[0x40100000 + player_index_offset] = 0x000000FF;  // -1: nobody signed in
    CHECK(!achievement_record_address(load));
    words[0x40100000 + player_index_offset] = 0x12345601;
    const auto address = achievement_record_address(load);
    CHECK(address && *address == 0x40100000 + 0x1FB0 + 0x30);
    words[player_table_pointer] = 0;
    CHECK(!achievement_record_address(load));
}

void toast_timing() {
    using namespace sfr;
    ToastQueue queue;
    CHECK(!queue.at(0.0));
    queue.push(15);
    queue.push(16);
    queue.push(15);  // already queued
    auto f = queue.at(10.0);
    CHECK(f && f->id == 15 && f->opacity == 0.0f && std::fabs(f->lift - 1.0f) < 1e-6f);
    f = queue.at(10.0 + ToastTiming::rise / 2);
    CHECK(f && f->opacity > 0.4f && f->opacity < 0.6f && f->lift > 0.0f && f->lift < 0.2f);
    f = queue.at(10.0 + ToastTiming::rise);
    CHECK(f && f->opacity == 1.0f && f->lift == 0.0f);
    f = queue.at(14.0);
    CHECK(f && f->id == 15 && f->opacity == 1.0f);
    f = queue.at(10.0 + ToastTiming::rise + ToastTiming::hold + ToastTiming::fade / 2);
    CHECK(f && f->opacity > 0.4f && f->opacity < 0.6f);
    // Gone after its five seconds, then a short gap, then the next.
    CHECK(ToastTiming::total > 4.5 && ToastTiming::total < 5.5);
    CHECK(!queue.at(10.0 + ToastTiming::total + 0.01));
    f = queue.at(10.0 + ToastTiming::total + ToastTiming::gap + 0.01);
    CHECK(f && f->id == 16 && f->opacity < 0.1f);
    CHECK(!queue.at(100.0));
    CHECK(queue.empty());
}

void page_navigation() {
    sfr::AchievementPageNav nav;
    nav.count = 50;
    nav.visible = 5;
    nav.move(7);
    CHECK(nav.selected == 7 && nav.first == 3);
    nav.page(-1);
    CHECK(nav.selected == 2 && nav.first == 2);
    nav.move(-100);
    CHECK(nav.selected == 0 && nav.first == 0);
    nav.move(1000);
    CHECK(nav.selected == 49 && nav.first == 45);
    sfr::AchievementPageNav empty;
    empty.count = 0;
    empty.move(3);
    CHECK(empty.selected == 0 && empty.first == 0);
    sfr::AchievementPageNav few;
    few.count = 3;
    few.move(5);
    CHECK(few.selected == 2 && few.first == 0);
}

void page_order() {
    sfr::TitleData title;
    for (uint16_t id : {6, 11, 12, 13, 14}) {
        sfr::TitleAchievement a;
        a.id = id;
        title.achievements.push_back(a);
    }
    sfr::UnlockedAchievements unlocked;
    unlocked.list = {{12, 100}, {6, 200}, {14, 200}};
    const auto order = sfr::achievement_page_order(title, unlocked);
    // Newest first (14 after 6 in the same second), then the locked in order.
    CHECK(order.size() == 5);
    CHECK(title.achievements[order[0]].id == 14 && title.achievements[order[1]].id == 6);
    CHECK(title.achievements[order[2]].id == 12);
    CHECK(title.achievements[order[3]].id == 11 && title.achievements[order[4]].id == 13);
}

void texts() {
    CHECK(std::string(sfr::achievement_texts(1).unlocked_toast) == "Achievement unlocked");
    CHECK(std::string(sfr::achievement_texts(0).title) == "Achievements");
    CHECK(std::string(sfr::achievement_texts(99).title) == "Achievements");
    CHECK(std::string(sfr::achievement_texts(3).title) == "Erfolge");
}

void canvas() {
    using namespace sfr;
    OverlayCanvas canvas(40, 20);
    canvas.fill_round_rect(0, 0, 40, 20, 6, overlay_rgb(0xFF0000, 0.5f));
    const auto& p = canvas.rgba();
    // Premultiplied: half red is 128, 0, 0, 128 in the middle.
    const size_t mid = (10 * 40 + 20) * 4;
    CHECK(std::abs(int(p[mid]) - 128) <= 1 && p[mid + 1] == 0 && std::abs(int(p[mid + 3]) - 128) <= 1);
    CHECK(p[3] < 40);  // the corner is cut by the rounding
    // An outline leaves the middle empty.
    OverlayCanvas ring(40, 40);
    ring.stroke_round_rect(0, 0, 40, 40, 8, 3, overlay_rgb(0xFFFFFF));
    CHECK(ring.rgba()[(20 * 40 + 20) * 4 + 3] == 0 && ring.rgba()[(20 * 40 + 1) * 4 + 3] > 200);
    // A picture is scaled in; grey makes it a dim grey.
    DecodedImage picture;
    picture.width = picture.height = 2;
    picture.rgba = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    OverlayCanvas shown(8, 8);
    shown.draw_image(picture, 0, 0, 8, 8);
    CHECK(shown.rgba()[(4 * 8 + 4) * 4] == 255 && shown.rgba()[(4 * 8 + 4) * 4 + 1] == 0);
    OverlayCanvas grey(8, 8);
    grey.draw_image(picture, 0, 0, 8, 8, 0, 1, true);
    const uint8_t* g = grey.rgba().data() + (4 * 8 + 4) * 4;
    CHECK(g[0] == g[1] && g[1] == g[2] && g[0] < 128);
}

void fonts_and_wrapping() {
    using namespace sfr;
    OverlayFonts fonts;
    fonts.load({}, {}, {});
    if (!fonts.any()) {
        std::printf("achievement_ui: no host font, text checks skipped\n");
        CHECK(fonts.measure("abc", 20, OverlayFonts::regular) == 0.0f);
        return;
    }
    const float one = fonts.measure("Bomb", 20, OverlayFonts::regular);
    CHECK(one > 10 && one < 80);
    CHECK(fonts.measure("Bomb", 40, OverlayFonts::regular) > one * 1.8f);
    const auto lines = wrap_overlay_text(fonts, "Hit rivals with a Target Torpedo 30 times.", 20, OverlayFonts::regular, 150, 4);
    CHECK(lines.size() >= 2);
    for (const auto& line : lines) CHECK(fonts.measure(line, 20, OverlayFonts::regular) <= 150.5f || line.find(' ') == std::string::npos);
    const auto cut = wrap_overlay_text(fonts, "one two three four five six seven eight nine ten", 20, OverlayFonts::regular, 80, 2);
    CHECK(cut.size() == 2 && cut[1].size() >= 3 && cut[1].substr(cut[1].size() - 3) == "...");
    OverlayCanvas canvas(200, 40);
    const float width = canvas.text(fonts, "A very long achievement name indeed", 0, 30, 20, overlay_rgb(0xFFFFFF),
                                    OverlayFonts::bold, 120);
    CHECK(width <= 121.0f);
    bool drawn = false;
    for (size_t i = 3; i < canvas.rgba().size(); i += 4) drawn = drawn || canvas.rgba()[i] > 0;
    CHECK(drawn);
}

void screenshot_blend() {
    using namespace sfr;
    auto image = std::make_shared<OverlayImage>();
    image->width = image->height = 1;
    image->rgba = {0, 0, 255, 255};  // opaque blue
    image->id = 1;
    set_screen_layer_source([image](float w, float h) {
        return std::vector<ScreenLayer>{{image, w / 4, h / 4, w / 2, h / 2, 1.0f}};
    });
    std::vector<uint8_t> bgra(8 * 4 * 4, 10);
    set_screen_layers_shown(false);
    blend_screen_layers(bgra, 8, 4, 0, 8);
    CHECK(bgra[(1 * 8 + 3) * 4] == 10);  // nothing while hidden
    set_screen_layers_shown(true);
    blend_screen_layers(bgra, 8, 4, 0, 8);
    CHECK(bgra[(1 * 8 + 3) * 4] == 255 && bgra[(1 * 8 + 3) * 4 + 2] == 0);  // blue in BGRA
    CHECK(bgra[0] == 10);
    set_screen_layer_source({});
    set_screen_layers_shown(false);
}
}

int main() {
    progress_from_record();
    record_address();
    toast_timing();
    page_navigation();
    page_order();
    texts();
    canvas();
    fonts_and_wrapping();
    screenshot_blend();
    if (failures) return 1;
    std::printf("achievement_ui: all passed\n");
    return 0;
}
