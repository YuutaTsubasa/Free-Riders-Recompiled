#pragma once

#include <algorithm>
#include <cmath>

namespace sfr::launcher {

// The launcher's layout is drawn for 1120 x 700 and scaled up to the screen.
constexpr float design_width = 1120.0f;
constexpr float design_height = 700.0f;

// Edges of a screen that something covers (a notch, the Dynamic Island, the
// home indicator), in the window's own units.
struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
};

// Where the launcher draws on a phone whose window measures points while the
// screen has more pixels (iOS: 3 pixels a point on an iPhone 15). The launcher
// works in pixels there, as on Android, so its fonts are drawn at the size
// they are shown: `x`, `y`, `width` and `height` are the safe area in pixels,
// and `scale` is the layout's scale in that space (Android's rule).
struct ScreenFit {
    float scale = 1.0f;
    float pixels_per_point = 1.0f;
    float x = 0, y = 0, width = 0, height = 0;
};

inline float layout_scale(float width, float height) {
    return (std::max)(1.0f, (std::min)(width / design_width, height / design_height));
}

inline ScreenFit fit_screen(int pixel_width, int pixel_height, int point_width, int point_height, Insets safe) {
    ScreenFit fit;
    if (pixel_width <= 0 || pixel_height <= 0) return fit;
    (void)point_height;  // pixels are square: the width gives the ratio
    fit.pixels_per_point = point_width > 0 ? float(pixel_width) / float(point_width) : 1.0f;
    const float ratio = fit.pixels_per_point;
    // Whole pixels, so the text stays sharp.
    const auto edge = [ratio](float inset) { return std::round((std::max)(0.0f, inset) * ratio); };
    // Never more than a quarter of the screen off either side: a broken
    // inset must not leave the launcher with no room.
    const float left = (std::min)(edge(safe.left), std::floor(pixel_width * 0.25f));
    const float right = (std::min)(edge(safe.right), std::floor(pixel_width * 0.25f));
    const float top = (std::min)(edge(safe.top), std::floor(pixel_height * 0.25f));
    const float bottom = (std::min)(edge(safe.bottom), std::floor(pixel_height * 0.25f));
    fit.x = left;
    fit.y = top;
    fit.width = float(pixel_width) - left - right;
    fit.height = float(pixel_height) - top - bottom;
    fit.scale = layout_scale(fit.width, fit.height);
    return fit;
}

}  // namespace sfr::launcher
