#include "screen_overlay.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace sfr {
namespace {
std::mutex source_mutex;
ScreenLayerSource layer_source;
std::atomic<bool> layers_shown{false};
std::function<void(const std::vector<ScreenTouch>&)> touch_sink;
}

void set_screen_layer_source(ScreenLayerSource source) {
    std::lock_guard lock(source_mutex);
    layer_source = std::move(source);
}
void set_screen_layers_shown(bool shown) { layers_shown.store(shown, std::memory_order_relaxed); }
bool screen_layers_shown() { return layers_shown.load(std::memory_order_relaxed); }

std::vector<ScreenLayer> screen_layers(float view_width, float view_height) {
    ScreenLayerSource source;
    {
        std::lock_guard lock(source_mutex);
        source = layer_source;
    }
    return source ? source(view_width, view_height) : std::vector<ScreenLayer>{};
}

void set_screen_touch_sink(std::function<void(const std::vector<ScreenTouch>&)> sink) {
    std::lock_guard lock(source_mutex);
    touch_sink = std::move(sink);
}
void screen_touches(const std::vector<ScreenTouch>& touches) {
    std::function<void(const std::vector<ScreenTouch>&)> sink;
    {
        std::lock_guard lock(source_mutex);
        sink = touch_sink;
    }
    if (sink) sink(touches);
}

void blend_screen_layers(std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t view_left, uint32_t view_width) {
    if (!screen_layers_shown() || bgra.size() < size_t(width) * height * 4 || view_left + view_width > width) return;
    for (ScreenLayer layer : screen_layers(float(view_width), float(height))) {
        layer.x += float(view_left);
        const OverlayImage* image = layer.image.get();
        if (!image || !image->width || !image->height || layer.width <= 0 || layer.height <= 0) continue;
        const int x0 = (std::max)(0, int(std::floor(layer.x))), y0 = (std::max)(0, int(std::floor(layer.y)));
        const int x1 = (std::min)(int(width), int(std::ceil(layer.x + layer.width)));
        const int y1 = (std::min)(int(height), int(std::ceil(layer.y + layer.height)));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                // Nearest texel of the picture under this pixel's centre.
                const float u = (float(x) + 0.5f - layer.x) / layer.width, v = (float(y) + 0.5f - layer.y) / layer.height;
                if (u < 0 || v < 0 || u >= 1 || v >= 1) continue;
                const uint32_t tx = (std::min)(image->width - 1, uint32_t(u * float(image->width)));
                const uint32_t ty = (std::min)(image->height - 1, uint32_t(v * float(image->height)));
                const uint8_t* src = image->rgba.data() + (size_t(ty) * image->width + tx) * 4;
                uint8_t* dst = bgra.data() + (size_t(y) * width + x) * 4;
                const float a = float(src[3]) / 255.0f * layer.opacity;
                const float k = layer.opacity;
                for (int c = 0; c < 3; ++c) {
                    const float s = float(src[2 - c]) * k;  // RGBA source into BGRA
                    dst[c] = uint8_t(std::clamp(s + float(dst[c]) * (1.0f - a) + 0.5f, 0.0f, 255.0f));
                }
            }
    }
}
}
