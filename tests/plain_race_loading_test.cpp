#include "plain_race_loading.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// Within two pixels: the framebuffer is an even number of pixels wide.
bool near(float a, float b) { return std::abs(a - b) < 2.0f; }

using sfr::plain_loading::View;
using sfr::plain_loading::corner_anchored;
using sfr::plain_loading::present_view;
using sfr::plain_loading::title_part_aspect;

bool fills(const View& view, uint32_t width, uint32_t height) {
    return near(view.x, 0) && near(view.y, 0) && near(view.width, float(width)) && near(view.height, float(height));
}

// Joka2010's report (#365): a 20:9 phone in Aspect Ratio Auto (a 2424x1080
// screen, the game's framebuffer 2424/1080*720 wide). After the race's files
// had loaded, the screen's black frames were the title's 16:9 view in the left
// of the wider framebuffer, shown as 16:9 and, with the screen's corner
// anchor, against the right edge: the touch controls and the frame rate moved
// into the right three quarters of the screen until the race.
void wide_phone_after_loading() {
    const uint32_t out_w = 2424, out_h = 1080, fb_w = 1616, fb_h = 720;
    const View before = present_view(out_w, out_h, fb_w, fb_h, true, 16.0f / 9.0f, true);
    require(near(before.width, 1920) && near(before.x, 504), "0.10.71: the black 16:9 frame went to the right edge");
    // Now: Now Loading is gone, so no corner, and the screen's frames take the framebuffer's shape.
    const bool corner = corner_anchored(true, false);
    require(!corner, "no corner once Now Loading is gone");
    const View after = present_view(out_w, out_h, fb_w, fb_h, true, title_part_aspect(true, false, fb_w, fb_h), corner);
    require(fills(after, out_w, out_h), "the screen's black frames fill the wide window");
    // The race's frames are the whole framebuffer.
    require(fills(present_view(out_w, out_h, fb_w, fb_h, false, 0.0f, false), out_w, out_h), "the race fills it");
}

// Every aspect: the frames of the screen, with Now Loading and without, and
// the race after it, fill the window when the framebuffer has its shape
// (Auto); with the title's 16:9 framebuffer (Original) the frame with Now
// Loading is against the right edge and the rest is centred.
void aspects() {
    struct Case { uint32_t out_w, out_h; };
    for (const Case c : {Case{1560, 702}, Case{2424, 1080}, Case{1680, 720}, Case{2560, 1080}, Case{1920, 1080},
                         Case{1280, 720}}) {
        // Auto: the framebuffer takes the window's shape at the title's height.
        const uint32_t fb_w = (std::max)(1280u, uint32_t(std::lround(720.0 * c.out_w / c.out_h)) & ~1u), fb_h = 720;
        for (const bool loading : {true, false}) {
            const bool corner = corner_anchored(true, loading);
            // The 880x720 back buffer of the loading screens is stretched over the whole frame.
            require(fills(present_view(c.out_w, c.out_h, fb_w, fb_h, true, 0.0f, corner), c.out_w, c.out_h),
                    "a stretched loading frame fills the window");
            if (fb_w > 1280 && !loading)
                require(fills(present_view(c.out_w, c.out_h, fb_w, fb_h, true, title_part_aspect(true, false, fb_w, fb_h),
                                           corner),
                              c.out_w, c.out_h),
                        "a black 16:9 view of the screen fills a wider window");
            if (fb_w > 1280 && loading) {
                // Codex review of b0f24de: Now Loading drawn in a 16:9 view keeps its shape, in the corner.
                const View shown = present_view(c.out_w, c.out_h, fb_w, fb_h, true,
                                                title_part_aspect(true, true, fb_w, fb_h), corner);
                require(near(shown.width / shown.height * 9.0f, 16.0f) && near(shown.height, float(c.out_h)) &&
                            near(shown.x + shown.width, float(c.out_w)),
                        "a 16:9 view with Now Loading stays 16:9, against the right edge");
            }
        }
        require(fills(present_view(c.out_w, c.out_h, fb_w, fb_h, false, 0.0f, false), c.out_w, c.out_h),
                "the race's first frames fill the window");
        // Off the screen the title's 16:9 views are still shown as centred 16:9.
        if (fb_w > 1280) {
            const View menu = present_view(c.out_w, c.out_h, fb_w, fb_h, true, title_part_aspect(false, false, fb_w, fb_h), false);
            require(near(menu.height, float(c.out_h)) && near(menu.width, c.out_h * 16.0f / 9.0f) &&
                        near(menu.x, (c.out_w - menu.width) / 2),
                    "a 16:9 menu stays centred 16:9");
        }
        // Original: a 1280x720 framebuffer in a wider window.
        const View loading = present_view(c.out_w, c.out_h, 1280, 720, false, 0.0f, corner_anchored(true, true));
        require(near(loading.x + loading.width, float(c.out_w)) && near(loading.y + loading.height, float(c.out_h)),
                "Original: Now Loading's frame is in the window's corner");
        const View black = present_view(c.out_w, c.out_h, 1280, 720, false, 0.0f, corner_anchored(true, false));
        require(near(black.x, (c.out_w - black.width) / 2), "Original: the rest of the screen is centred");
        const View race = present_view(c.out_w, c.out_h, 1280, 720, false, 0.0f, corner_anchored(false, true));
        require(near(race.x, (c.out_w - race.width) / 2) && near(race.y, (c.out_h - race.height) / 2),
                "Original: the race is centred");
    }
}

void anchor() {
    require(corner_anchored(true, true), "Now Loading on the screen: the corner");
    require(!corner_anchored(true, false) && !corner_anchored(false, true) && !corner_anchored(false, false),
            "anything else: centred");
    require(title_part_aspect(false, false, 1600, 720) == 16.0f / 9.0f, "off the screen a title view stays 16:9");
    require(near(title_part_aspect(true, false, 1600, 720) * 720.0f, 1600.0f), "a black one: the framebuffer's shape");
    require(title_part_aspect(true, true, 1600, 720) == 16.0f / 9.0f, "one with Now Loading stays 16:9");
    require(title_part_aspect(true, false, 0, 0) == 16.0f / 9.0f, "no framebuffer yet: 16:9");
    require(fills(present_view(0, 0, 1280, 720, false, 0.0f, false), 0, 0), "a minimised window has no view");
}

// The sounds asked for while the screen is up (the run of #365's follow-up:
// Omochao's 200000 and 200003 and the effect 100000, players 19 to 23) are
// left out; those asked for once it is gone (the race's 401013 on players 295
// and 296, the same present) and the title's own "no sound" are not.
void sounds() {
    using sfr::plain_loading::silences_sound;
    for (const int32_t sound : {200000, 100000, 200003, 0})
        require(silences_sound(true, sound), "the practice's sounds are left out");
    for (const int32_t sound : {401013, 200000, 100002, 3, 22})
        require(!silences_sound(false, sound), "the race's and the menus' sounds play");
    require(!silences_sound(true, -1) && !silences_sound(false, -1), "no sound stays no sound");
}
}  // namespace

int main() {
    try {
        wide_phone_after_loading();
        aspects();
        anchor();
        sounds();
        std::cout << "plain race loading tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
