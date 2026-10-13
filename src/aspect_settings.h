#pragma once

#include <cstdint>
#include <cmath>

// Aspect-ratio settings (widescreen beyond 16:9), read once from the
// environment the launcher hands over:
//   SFR_ASPECT_MODE = auto (default, also when absent) | original
//     auto: the logical back buffer takes the shape of the window (or of the
//       display in full screen): the race's 3D is widened (hor+), the HUD is
//       laid out per SFR_UI_ALIGN, the menus' backdrop fills the width and the
//       rest of the 16:9 screens stays in a centered 16:9 area. A window that
//       is 16:9 or narrower keeps the title's own 16:9 back buffer.
//     original: the title's own 16:9 back buffer and presentation, untouched
//       (every override below is ignored).
//   SFR_ASPECT_RATIO = <num>:<den>  (auto mode only, optional; "21:9", "32:9", "16:3")
//     the logical width comes from this ratio at the title's height,
//     even(round(720 * num / den)), whatever the window's or display's shape;
//     a window of another shape is letterboxed (black bars, proportions kept).
//     Absent or invalid: the shape of the window/display is used (auto).
//   SFR_UI_ALIGN = edge (default) | center
//     edge: the race HUD's side groups go to the left/right edges of the frame.
//     center: the whole HUD stays in the centered 16:9 area.
// Explicit overrides (auto mode only):
//   SFR_ASPECT_WIDTH=<n>  forces the logical width (1280..8192).
//   SFR_HUD_ANCHOR=0|1    HUD edge anchoring (otherwise from SFR_UI_ALIGN).
//   SFR_MENU_BG=0|1       menus' backdrop over the whole frame (default 1).
//   SFR_UI_CENTER=0|1     race HUD in the centered 16:9 area (default 1).
namespace sfr::aspect {

// The title's own layout: 2D is laid out for 1280 x 720 (16:9).
inline constexpr uint32_t title_layout_width = 1280;
inline constexpr uint32_t title_layout_height = 720;

[[nodiscard]] bool original_mode();
[[nodiscard]] bool hud_anchor();
[[nodiscard]] bool ui_center();
[[nodiscard]] bool menu_backdrop();

// The logical back-buffer width to create for a title asking for
// guest_width x guest_height (create_device and the title's own copy of the
// presentation parameters must agree). Decided once, on the first call, and
// logged; later calls with the same request answer the same.
[[nodiscard]] uint32_t logical_width(uint32_t guest_width, uint32_t guest_height);

// The calibration screens set an 880-wide view the console's scaler
// stretches to the 16:9 display.
inline constexpr uint32_t title_calibration_width = 880;
// A view the title lays out as a 16:9 screen (menus, loading, calibration),
// as opposed to one that fills a wider back buffer (a race).
[[nodiscard]] constexpr bool title_16x9_view(uint32_t view_width) noexcept {
    return view_width == title_layout_width || view_width == title_calibration_width;
}

// The width of the centered 16:9 area of a frame logical_height tall.
[[nodiscard]] constexpr uint32_t center_width(uint32_t logical_height) noexcept {
    return (logical_height * 16 + 4) / 9;
}
// Parses "<num>:<den>" (both 1..9999, as plain digits). False when invalid.
[[nodiscard]] inline bool parse_ratio(const char* text, uint32_t& num, uint32_t& den) noexcept {
    if (!text) return false;
    uint32_t parts[2] = {0, 0};
    int index = 0;
    int digits = 0;
    for (const char* c = text; *c; ++c) {
        if (*c >= '0' && *c <= '9') {
            parts[index] = parts[index] * 10 + uint32_t(*c - '0');
            if (++digits > 4) return false;
        } else if (*c == ':' && index == 0 && digits > 0) {
            index = 1;
            digits = 0;
        } else {
            return false;
        }
    }
    if (index != 1 || digits == 0 || parts[0] == 0 || parts[1] == 0) return false;
    num = parts[0];
    den = parts[1];
    return true;
}

// The logical width of a num:den frame guest_height tall: even, never narrower
// than guest_width (a 16:9 or narrower ratio keeps the title's own width) and
// never wider than `most`.
[[nodiscard]] inline uint32_t width_for_ratio(uint32_t guest_width, uint32_t guest_height,
                                              uint32_t num, uint32_t den, uint32_t most) noexcept {
    const double exact = double(guest_height) * double(num) / double(den);
    uint32_t width = uint32_t(std::llround(exact / 2.0)) * 2u;
    if (width > most) width = most & ~1u;
    if (width < guest_width) width = guest_width;
    if (width <= center_width(guest_height) + 8) width = guest_width;
    return width;
}

// Whether a frame is wider than 16:9 (by more than rounding).
[[nodiscard]] constexpr bool wider_than_16x9(uint32_t width, uint32_t height) noexcept {
    return width > center_width(height) + 8;
}

}
