#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace sfr {
// Pictures the port draws over the presented frame on its own (Issue #288:
// the achievement notification and the achievements page), apart from the
// touch controls and readouts the blit shader draws. Whoever makes them
// (achievement_overlay.h) registers a source; the presentation asks it for
// the layers of each frame it draws, with the size of the game picture in
// the window, and draws them over it in order (screen_overlay_renderer.h).
struct OverlayImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;  // premultiplied by alpha, rows top down
    uint64_t id = 0;            // a new picture gets a new id
};
struct ScreenLayer {
    std::shared_ptr<const OverlayImage> image;
    // Where, in pixels of the game picture as shown in the window (0,0 its
    // top left), and how opaque (0..1).
    float x = 0, y = 0, width = 0, height = 0;
    float opacity = 1;
};
using ScreenLayerSource = std::function<std::vector<ScreenLayer>(float view_width, float view_height)>;

// Set once (or cleared with an empty function) by the game side.
void set_screen_layer_source(ScreenLayerSource source);
// Whether there is anything to draw now: the source keeps it up to date, so
// the presentation can take the drawing path only while it is.
void set_screen_layers_shown(bool shown);
bool screen_layers_shown();
// The layers for a frame (empty without a source).
std::vector<ScreenLayer> screen_layers(float view_width, float view_height);
// Blends the layers into a BGRA picture of the game frame (SFR_SCREENSHOT),
// as the presentation draws them over the window's picture: the game
// picture is columns view_left .. view_left + view_width of it.
void blend_screen_layers(std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t view_left, uint32_t view_width);

// Touches for the overlay (achievement_overlay.h), in game-picture
// fractions, each frame the presentation reads them.
struct ScreenTouch { int64_t id; float x, y; };
void set_screen_touch_sink(std::function<void(const std::vector<ScreenTouch>&)> sink);
void screen_touches(const std::vector<ScreenTouch>& touches);
}
