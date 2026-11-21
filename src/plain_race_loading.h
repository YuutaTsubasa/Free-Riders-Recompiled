#pragma once

#include <algorithm>
#include <cstdint>

// The plain race loading screen (Issue #365, docs/plain-race-loading.md):
// the decisions that do not need the game, kept here so they can be tested.
namespace sfr::plain_loading {

// Where the presentation puts the shown frame in the window (the blit's
// viewport), in window pixels.
struct View {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

// The framebuffer (fb_width x fb_height) is scaled into the window keeping
// its shape. A part of it (the title's 16:9 screens drawn in the left of a
// wider framebuffer) shown at display_aspect keeps the height it has inside
// the whole framebuffer, never more than the window holds, at that aspect.
// corner puts the result in the window's bottom right corner instead of its
// centre.
[[nodiscard]] inline View present_view(uint32_t out_width, uint32_t out_height, uint32_t fb_width,
                                       uint32_t fb_height, bool part, float display_aspect, bool corner) {
    View view;
    if (!out_width || !out_height || !fb_width || !fb_height) return view;
    const float scale = (std::min)(float(out_width) / float(fb_width), float(out_height) / float(fb_height));
    view.width = float(fb_width) * scale;
    view.height = float(fb_height) * scale;
    if (part && display_aspect > 0.0f) {
        view.height = (std::min)(view.height, float(out_width) / display_aspect);
        view.width = view.height * display_aspect;
    }
    const float at = corner ? 1.0f : 0.5f;
    view.x = (float(out_width) - view.width) * at;
    view.y = (float(out_height) - view.height) * at;
    return view;
}

// The shape a 16:9 screen of the title, drawn in the left of a framebuffer
// wider than 16:9, is shown at. Normally 16:9, centred with black at the
// sides. While the plain loading screen is up, a frame without Now Loading
// is all black (none of the practice reaches the GPU), so it fills the
// framebuffer's shape instead: the window stays filled from the screen to
// the race, and the touch controls and the frame-rate readout, which are
// drawn in the shown frame, do not jump into a narrower box and back. A
// frame with Now Loading keeps 16:9, so its words and ring keep their shape
// (it goes to the corner, corner_anchored).
[[nodiscard]] inline float title_part_aspect(bool screen_up, bool now_loading_drawn, uint32_t fb_width,
                                             uint32_t fb_height) {
    if (screen_up && !now_loading_drawn && fb_width && fb_height) return float(fb_width) / float(fb_height);
    return 16.0f / 9.0f;
}

// Whether the frame goes to the window's bottom right corner: only a frame of
// the screen in which Now Loading was drawn (it then is the only thing in it,
// and in the corner it sits in the window's corner on a wider window). Once
// the files are loaded Now Loading goes, and the rest of the screen and the
// race are placed as always. Before 0.10.72 the whole screen was anchored, so
// on a wide phone the black 16:9 frames after the loading pushed the touch
// controls into the right part of the screen (Joka2010, #365).
[[nodiscard]] constexpr bool corner_anchored(bool screen_up, bool now_loading_drawn) noexcept {
    return screen_up && now_loading_drawn;
}

// Whether a sound the title asks its sound manager for (8222FA18: player,
// sound) is left out: every one asked for while the screen is up, which are
// the practice's (Omochao's lines and its effects); -1 is the title's own
// "no sound" and stays as it is. The title plays the music through another
// path, which is left alone, and the race's first sounds are asked for once
// the practice object is gone.
[[nodiscard]] constexpr bool silences_sound(bool screen_up, int32_t sound) noexcept {
    return screen_up && sound != -1;
}

}  // namespace sfr::plain_loading
