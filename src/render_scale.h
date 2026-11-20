#pragma once

#include <algorithm>
#include <cstdint>

// The internal rendering scale (SFR_RENDER_SCALE): a whole percent of the
// title's logical back buffer (1280x720). The launcher derives it from the window (or display)
// height and its resolution scale; started without the launcher, the variable
// keeps this meaning.
namespace sfr::render_scale {

inline constexpr uint32_t min_percent = 25;
// 8x of 720 lines; the size limit below usually lowers it first.
inline constexpr uint32_t max_percent = 800;
inline constexpr uint32_t default_percent = 100;
// The largest side of a scaled render target (and of the logical back buffer):
// within every device the renderer supports, and the memory a target takes.
inline constexpr uint32_t max_extent = 8192;

// SFR_RENDER_SCALE's text: plain digits, clamped to min..max_percent. Absent,
// empty or not a number gives the default; `valid` says which it was.
[[nodiscard]] inline uint32_t parse_percent(const char* text, bool* valid = nullptr) noexcept {
    if (valid) *valid = true;
    if (!text || !*text) return default_percent;
    uint64_t value = 0;
    for (const char* c = text; *c; ++c) {
        if (*c < '0' || *c > '9' || value > 100000) {
            if (valid) *valid = false;
            return default_percent;
        }
        value = value * 10 + uint64_t(*c - '0');
    }
    return uint32_t(std::clamp<uint64_t>(value, min_percent, max_percent));
}

// The percent lowered (never raised) so that neither side of a
// logical_width x logical_height buffer scaled by it exceeds max_extent.
[[nodiscard]] inline uint32_t fit_percent(uint32_t percent, uint32_t logical_width, uint32_t logical_height) noexcept {
    const uint32_t side = (std::max)({logical_width, logical_height, 1u});
    const uint32_t most = uint32_t(uint64_t(max_extent) * 100 / side);
    return (std::max)(1u, (std::min)(percent, most));
}

// The percent of the title's logical_height lines that draws an output
// (the window, or the display in full screen) output_height lines tall at
// the launcher's Render Size: resolution_scale is a percent of the output's
// height, or 0 for Original, which is 100 whatever the output. The launcher
// starts the game with it, and the game follows a resized window with it
// (SFR_RENDER_SIZE, Issue #365).
[[nodiscard]] inline uint32_t percent_of_output(uint32_t output_height, uint32_t resolution_scale,
                                                uint32_t logical_height = 720) noexcept {
    if (!resolution_scale || !logical_height) return default_percent;
    const uint64_t scaled = uint64_t(output_height) * resolution_scale;
    return uint32_t(std::clamp<uint64_t>((scaled + logical_height / 2) / logical_height, min_percent, max_percent));
}

// One side scaled by percent, to the nearest pixel; an even side stays even
// (nearest even), so internal targets never get an odd size from an even
// logical one.
[[nodiscard]] inline uint32_t scaled_extent(uint32_t logical, uint32_t percent) noexcept {
    if (logical % 2 == 0) return (std::max)(2u, uint32_t((uint64_t(logical) * percent + 100) / 200 * 2));
    return (std::max)(1u, uint32_t((uint64_t(logical) * percent + 50) / 100));
}

}
