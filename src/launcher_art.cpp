#include "launcher_art.h"
#include <algorithm>
#include <cmath>

namespace sfr {
namespace {
struct Color { float r, g, b, a; };

// Distance from p to the segment a-b.
float segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return std::hypot(px - ax - dx * t, py - ay - dy * t);
}

// "Over" with straight alpha.
Color over(Color top, Color under) {
    const float a = top.a + under.a * (1.0f - top.a);
    if (a <= 0.0f) return {0, 0, 0, 0};
    const auto channel = [&](float t, float u) { return (t * top.a + u * under.a * (1.0f - top.a)) / a; };
    return {channel(top.r, under.r), channel(top.g, under.g), channel(top.b, under.b), a};
}

// The icon at one point of the unit square.
Color sample(float x, float y) {
    Color color{0, 0, 0, 0};
    const float from_centre = std::hypot(x - 0.5f, y - 0.5f);
    if (from_centre < 0.47f) {
        // Deep blue at the top to violet at the bottom, lighter towards the top left.
        const float t = y;
        const float light = std::max(0.0f, 0.35f - std::hypot(x - 0.35f, y - 0.3f)) * 0.8f;
        color = {0.10f + 0.18f * t + light, 0.32f - 0.20f * t + light, 0.92f - 0.30f * t + light, 1.0f};
    }
    if (std::abs(from_centre - 0.455f) < 0.022f) color = over({0.40f, 0.92f, 1.0f, 1.0f}, color);
    // Speed lines behind the board.
    const float lines[3][4] = {{0.10f, 0.50f, 0.30f, 0.44f}, {0.14f, 0.63f, 0.32f, 0.575f}, {0.20f, 0.75f, 0.36f, 0.70f}};
    for (const auto& l : lines)
        if (segment_distance(x, y, l[0], l[1], l[2], l[3]) < 0.02f) color = over({1, 1, 1, 0.85f}, color);
    // The board, with a cyan stripe along it.
    const float board = segment_distance(x, y, 0.30f, 0.66f, 0.80f, 0.40f);
    if (board < 0.085f) color = over({0.97f, 0.98f, 1.0f, 1.0f}, color);
    if (board < 0.032f) color = over({0.25f, 0.85f, 1.0f, 1.0f}, color);
    return color;
}
}

std::vector<uint32_t> render_icon(int size) {
    std::vector<uint32_t> pixels(size_t(size) * size_t(size));
    constexpr int grid = 4;  // 16 samples a pixel for smooth edges
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < grid; ++sy)
                for (int sx = 0; sx < grid; ++sx) {
                    const Color c = sample((float(px) + (sx + 0.5f) / grid) / float(size),
                                           (float(py) + (sy + 0.5f) / grid) / float(size));
                    r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
                }
            const float samples = float(grid * grid);
            const auto byte = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            const float alpha = a / samples;
            const uint32_t out = a > 0 ? (byte(alpha) << 24 | byte(r / a) << 16 | byte(g / a) << 8 | byte(b / a)) : 0;
            pixels[size_t(py) * size_t(size) + size_t(px)] = out;
        }
    return pixels;
}
}
