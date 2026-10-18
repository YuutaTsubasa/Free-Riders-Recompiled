#include "launcher_screen_fit.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

using sfr::launcher::fit_screen;
using sfr::launcher::Insets;
using sfr::launcher::layout_scale;

void android_rule() {
    // The rule Android has used since the first release: fit 1120 x 700, never below 1.
    require(near(layout_scale(2400, 1080), 1080.0f / 700.0f), "a 20:9 phone fits by its height");
    require(near(layout_scale(1120, 700), 1.0f), "the design size is scale 1");
    require(near(layout_scale(800, 480), 1.0f), "a small screen still draws at scale 1");
    require(near(layout_scale(2048, 1536), 2048.0f / 1120.0f), "a 4:3 screen fits by its width");
}

void iphone_15() {
    // Issue #185: 852 x 393 points, 2556 x 1179 pixels, the Dynamic Island
    // and rounded corners 59 points off each side, the home indicator 21.
    const auto fit = fit_screen(2556, 1179, 852, 393, Insets{59, 0, 59, 21});
    require(near(fit.pixels_per_point, 3.0f), "three pixels a point");
    require(near(fit.x, 177.0f) && near(fit.y, 0.0f), "the safe area starts past the island");
    require(near(fit.width, 2556.0f - 354.0f) && near(fit.height, 1179.0f - 63.0f), "the safe area in pixels");
    require(near(fit.scale, (1179.0f - 63.0f) / 700.0f), "the layout fits the safe height");
    // The layout of 1120 x 700 at that scale fits on the screen.
    require(1120.0f * fit.scale <= fit.width + 0.5f && 700.0f * fit.scale <= fit.height + 0.5f, "the layout fits");
    // As big against the screen as on a 20:9 Android phone, not twice as big.
    const float android_share = 700.0f * layout_scale(2400, 1080) / 1080.0f;
    const float iphone_share = 700.0f * fit.scale / 1179.0f;
    require(iphone_share <= android_share + 0.001f && iphone_share > android_share * 0.9f, "in Android's proportion");
}

void no_insets() {
    const auto fit = fit_screen(2732, 2048, 1366, 1024, Insets{});
    require(near(fit.pixels_per_point, 2.0f), "an iPad has two pixels a point");
    require(near(fit.x, 0) && near(fit.y, 0) && near(fit.width, 2732) && near(fit.height, 2048), "the whole screen");
    require(near(fit.scale, 2732.0f / 1120.0f), "an iPad fits by its width");
}

void odd_input() {
    const auto none = fit_screen(0, 0, 0, 0, Insets{});
    require(near(none.scale, 1.0f) && near(none.width, 0), "no screen yet: scale 1, nothing to draw");
    const auto unknown_points = fit_screen(2556, 1179, 0, 0, Insets{59, 0, 59, 21});
    require(near(unknown_points.pixels_per_point, 1.0f), "no point size: one pixel a point");
    const auto huge = fit_screen(2556, 1179, 852, 393, Insets{1000, 1000, 1000, 1000});
    require(near(huge.x, 639.0f) && near(huge.width, 2556.0f - 2 * 639.0f), "an inset is at most a quarter");
    require(near(huge.y, 294.0f) && near(huge.height, 1179.0f - 2 * 294.0f), "and so is a top or bottom one");
    const auto negative = fit_screen(2556, 1179, 852, 393, Insets{-5, -5, -5, -5});
    require(near(negative.x, 0) && near(negative.width, 2556), "a negative inset counts as none");
    const auto fraction = fit_screen(2556, 1179, 852, 393, Insets{47.33f, 0, 47.33f, 0});
    require(near(fraction.x, std::round(fraction.x)), "whole pixels");
}
}  // namespace

int main() {
    try {
        android_rule();
        iphone_15();
        no_insets();
        odd_input();
    } catch (const std::exception& error) {
        std::cerr << "launcher_screen_fit_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "launcher_screen_fit_test passed\n";
}
