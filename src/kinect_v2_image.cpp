#include "kinect_v2_image.h"

#include <cstddef>
#include <algorithm>
#include <cmath>

namespace sfr {
namespace {
// The span of source pixels one target pixel covers along an axis: whole
// pixels from first up to (not including) last, at least one. A hair of
// slack, so that an exact third (1920 to 640) lands on exact pixels.
void span(double from, double to, uint32_t size, uint32_t& first, uint32_t& last) {
    const double low = std::floor(from + 1e-3), high = std::ceil(to - 1e-3);
    first = uint32_t(std::clamp(low, 0.0, double(size - 1)));
    last = uint32_t(std::clamp(high, double(first + 1), double(size)));
}

// The common view's fit for a frame of any size (the crop stays the same part
// of the image): source pixel to preview pixel.
KinectFit frame_fit(KinectImageKind kind, uint32_t width, uint32_t height, KinectV2View view) {
    const bool colour = kind == KinectImageKind::colour;
    const KinectV2Crop whole = kinect_v2_crop(kind, view);
    const float fx = float(width) / float(colour ? kinect_v2_colour_width : kinect_v2_depth_width);
    const float fy = float(height) / float(colour ? kinect_v2_colour_height : kinect_v2_depth_height);
    const KinectV2Crop crop{whole.x * fx, whole.y * fy, whole.width * fx, whole.height * fy};
    const KinectImageSize size = kinect_v2_preview_size(kind, view);
    return kinect_fit_crop(crop, size.width, size.height);
}

// Colour fitted into the preview's image: each target pixel the average
// of the source pixels under it; sample(x, y) gives one source pixel as
// B, G, R, 0.
template<class Sample>
void fit_colour(uint32_t width, uint32_t height, KinectV2View view, KinectImage& out, Sample sample) {
    const KinectImageSize size = kinect_v2_preview_size(KinectImageKind::colour, view);
    const uint32_t tw = size.width, th = size.height;
    out.width = tw;
    out.height = th;
    out.bytes_per_pixel = 4;
    out.pixels.assign(size_t(tw) * th * 4, 0);
    if (!width || !height) return;
    const KinectFit fit = frame_fit(KinectImageKind::colour, width, height, view);
    for (uint32_t ty = 0; ty < th; ++ty) {
        const double sy = (ty + 0.5 - fit.offset_y) / fit.scale;
        if (sy < 0 || sy >= height) continue;  // a bar
        uint32_t y0, y1;
        span((ty - fit.offset_y) / fit.scale, (ty + 1 - fit.offset_y) / fit.scale, height, y0, y1);
        for (uint32_t tx = 0; tx < tw; ++tx) {
            const double sx = (tx + 0.5 - fit.offset_x) / fit.scale;
            if (sx < 0 || sx >= width) continue;
            uint32_t x0, x1;
            span((tx - fit.offset_x) / fit.scale, (tx + 1 - fit.offset_x) / fit.scale, width, x0, x1);
            uint32_t b = 0, g = 0, r = 0, count = 0;
            for (uint32_t y = y0; y < y1; ++y)
                for (uint32_t x = x0; x < x1; ++x) {
                    const uint32_t pixel = sample(x, y);
                    b += pixel & 0xFF;
                    g += pixel >> 8 & 0xFF;
                    r += pixel >> 16 & 0xFF;
                    ++count;
                }
            uint8_t* to = &out.pixels[(size_t(ty) * tw + tx) * 4];
            to[0] = uint8_t((b + count / 2) / count);
            to[1] = uint8_t((g + count / 2) / count);
            to[2] = uint8_t((r + count / 2) / count);
            to[3] = 0;
        }
    }
}

bool sane(const KinectImagePoint& point, uint32_t width, uint32_t height) {
    // Only a guard against absurd coordinates (they would overflow the
    // window's integer maths). A point outside the image is still placed, so
    // that a bone from one inside still reaches the edge: the preview window
    // clips its drawing to the picture, and draws a joint only when its
    // centre is inside.
    return point.x > -float(width) && point.x < 2.0f * float(width) && point.y > -float(height) &&
           point.y < 2.0f * float(height);
}
}

KinectFit kinect_fit(uint32_t source_width, uint32_t source_height, uint32_t target_width, uint32_t target_height) {
    KinectFit fit;
    if (!source_width || !source_height) return fit;
    fit.scale = std::min(float(target_width) / float(source_width), float(target_height) / float(source_height));
    fit.offset_x = (float(target_width) - float(source_width) * fit.scale) / 2.0f;
    fit.offset_y = (float(target_height) - float(source_height) * fit.scale) / 2.0f;
    return fit;
}

KinectImagePoint kinect_fit_point(const KinectFit& fit, float x, float y, uint32_t target_width,
                                  uint32_t target_height) {
    if (!std::isfinite(x) || !std::isfinite(y) || !(fit.scale > 0)) return {};
    KinectImagePoint point{fit.offset_x + x * fit.scale, fit.offset_y + y * fit.scale, true};
    point.visible = sane(point, target_width, target_height);
    return point;
}

KinectV2Crop kinect_v2_crop(KinectImageKind kind, KinectV2View view) {
    const bool colour = kind == KinectImageKind::colour;
    if (view == KinectV2View::original)
        return {0, 0, float(colour ? kinect_v2_colour_width : kinect_v2_depth_width),
                float(colour ? kinect_v2_colour_height : kinect_v2_depth_height)};
    const float colour_half_x = float(kinect_v2_colour_width) / 2.0f / kinect_v2_colour_focal;
    const float colour_half_y = float(kinect_v2_colour_height) / 2.0f / kinect_v2_colour_focal;
    const float depth_half_x = float(kinect_v2_depth_width) / 2.0f / kinect_v2_depth_focal;
    const float depth_half_y = float(kinect_v2_depth_height) / 2.0f / kinect_v2_depth_focal;
    // The intersection of the two fields of view, whatever its shape.
    const float half_x = std::min(colour_half_x, depth_half_x);
    const float half_y = std::min(colour_half_y, depth_half_y);
    const float focal = colour ? kinect_v2_colour_focal : kinect_v2_depth_focal;
    const float centre_x = colour ? kinect_v2_colour_centre_x : kinect_v2_depth_centre_x;
    const float centre_y = colour ? kinect_v2_colour_centre_y : kinect_v2_depth_centre_y;
    return {centre_x - half_x * focal, centre_y - half_y * focal, 2.0f * half_x * focal, 2.0f * half_y * focal};
}

KinectFit kinect_fit_crop(const KinectV2Crop& crop, uint32_t target_width, uint32_t target_height) {
    KinectFit fit;
    if (!(crop.width > 0) || !(crop.height > 0)) return fit;
    fit.scale = std::min(float(target_width) / crop.width, float(target_height) / crop.height);
    fit.offset_x = (float(target_width) - crop.width * fit.scale) / 2.0f - crop.x * fit.scale;
    fit.offset_y = (float(target_height) - crop.height * fit.scale) / 2.0f - crop.y * fit.scale;
    return fit;
}

KinectImageSize kinect_v2_preview_size(KinectImageKind kind, KinectV2View view) {
    const bool colour = kind == KinectImageKind::colour;
    const KinectV2Crop crop = kinect_v2_crop(kind, view);
    const uint32_t cap = colour ? kinect_v2_colour_preview_long_side : kinect_v2_depth_preview_long_side;
    // The long side is the cap, the short one follows the crop's shape.
    if (crop.width >= crop.height)
        return {cap, std::max(1u, uint32_t(std::lround(double(cap) * crop.height / crop.width)))};
    return {std::max(1u, uint32_t(std::lround(double(cap) * crop.width / crop.height))), cap};
}

KinectImageRect kinect_v2_picture_rect(KinectImageKind kind, KinectV2View view) {
    // The image is the crop (no bars), so the picture is all of it.
    const KinectImageSize size = kinect_v2_preview_size(kind, view);
    return {0, 0, float(size.width), float(size.height)};
}

uint32_t kinect_yuy2_bgrx(uint8_t y, uint8_t u, uint8_t v) {
    const int c = int(y) - 16, d = int(u) - 128, e = int(v) - 128;
    const auto clamp = [](int value) { return uint32_t(std::clamp(value, 0, 255)); };
    const uint32_t r = clamp((298 * c + 409 * e + 128) >> 8);
    const uint32_t g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
    const uint32_t b = clamp((298 * c + 516 * d + 128) >> 8);
    return b | g << 8 | r << 16;
}

void kinect_v2_colour_from_yuy2(const uint8_t* yuy2, uint32_t width, uint32_t height, KinectV2View view,
                                KinectImage& out) {
    const size_t stride = size_t(width) * 2;
    fit_colour(width, height, view, out, [yuy2, stride](uint32_t x, uint32_t y) {
        const uint8_t* pair = yuy2 + size_t(y) * stride + size_t(x & ~1u) * 2;
        return kinect_yuy2_bgrx(pair[(x & 1) ? 2 : 0], pair[1], pair[3]);
    });
}

void kinect_v2_colour_from_bgra(const uint8_t* bgra, uint32_t width, uint32_t height, KinectV2View view,
                                KinectImage& out) {
    const size_t stride = size_t(width) * 4;
    fit_colour(width, height, view, out, [bgra, stride](uint32_t x, uint32_t y) {
        const uint8_t* pixel = bgra + size_t(y) * stride + size_t(x) * 4;
        return uint32_t(pixel[0]) | uint32_t(pixel[1]) << 8 | uint32_t(pixel[2]) << 16;
    });
}

void kinect_v2_depth_image(const uint16_t* millimetres, const uint8_t* body_index, uint32_t width,
                           uint32_t height, KinectV2View view, KinectImage& out) {
    const KinectImageSize size = kinect_v2_preview_size(KinectImageKind::depth_and_player, view);
    const uint32_t tw = size.width, th = size.height;
    out.width = tw;
    out.height = th;
    out.bytes_per_pixel = 2;
    out.pixels.assign(size_t(tw) * th * 2, 0);
    if (!width || !height) return;
    const KinectFit fit = frame_fit(KinectImageKind::depth_and_player, width, height, view);
    for (uint32_t ty = 0; ty < th; ++ty) {
        const double sy = std::floor((ty + 0.5 - fit.offset_y) / fit.scale);
        if (sy < 0 || sy >= height) continue;
        for (uint32_t tx = 0; tx < tw; ++tx) {
            const double sx = std::floor((tx + 0.5 - fit.offset_x) / fit.scale);
            if (sx < 0 || sx >= width) continue;
            const size_t at = size_t(sy) * width + size_t(sx);
            const uint32_t depth = std::min<uint32_t>(millimetres[at], 0x1FFF);
            const uint32_t body = body_index ? body_index[at] : kinect_v2_no_body;
            const uint16_t value = uint16_t(depth << 3 | (body < 6 ? body + 1 : 0));
            uint8_t* to = &out.pixels[(size_t(ty) * tw + tx) * 2];
            to[0] = uint8_t(value);
            to[1] = uint8_t(value >> 8);
        }
    }
}

KinectImagePoint kinect_v2_depth_to_preview(float x, float y, KinectV2View view) {
    return kinect_fit_point(frame_fit(KinectImageKind::depth_and_player, kinect_v2_depth_width, kinect_v2_depth_height, view),
                            x, y, kinect_v2_preview_size(KinectImageKind::depth_and_player, view).width,
                            kinect_v2_preview_size(KinectImageKind::depth_and_player, view).height);
}

KinectImagePoint kinect_v2_colour_to_preview(float x, float y, KinectV2View view) {
    return kinect_fit_point(frame_fit(KinectImageKind::colour, kinect_v2_colour_width, kinect_v2_colour_height, view), x, y,
                            kinect_v2_preview_size(KinectImageKind::colour, view).width,
                            kinect_v2_preview_size(KinectImageKind::colour, view).height);
}

// Typical intrinsics of a v2 (kinect_v2_depth_focal, kinect_v2_colour_focal).
KinectImagePoint kinect_v2_depth_estimate(const std::array<float, 3>& p, KinectV2View view) {
    if (!(p[2] > 0.1f)) return {};
    return kinect_v2_depth_to_preview(kinect_v2_depth_centre_x + p[0] * kinect_v2_depth_focal / p[2],
                                     kinect_v2_depth_centre_y - p[1] * kinect_v2_depth_focal / p[2], view);
}

KinectImagePoint kinect_v2_colour_estimate(const std::array<float, 3>& p, KinectV2View view) {
    if (!(p[2] > 0.1f)) return {};
    return kinect_v2_colour_to_preview(kinect_v2_colour_centre_x + p[0] * kinect_v2_colour_focal / p[2],
                                      kinect_v2_colour_centre_y - p[1] * kinect_v2_colour_focal / p[2], view);
}

KinectImagePoint kinect_v2_place(KinectImageKind kind, const std::array<float, 3>& p, const float* mapped,
                                 KinectV2View view) {
    const bool colour = kind == KinectImageKind::colour;
    if (mapped && std::isfinite(mapped[0]) && std::isfinite(mapped[1]))
        return colour ? kinect_v2_colour_to_preview(mapped[0], mapped[1], view)
                      : kinect_v2_depth_to_preview(mapped[0], mapped[1], view);
    return colour ? kinect_v2_colour_estimate(p, view) : kinect_v2_depth_estimate(p, view);
}

void kinect_colour_from_positions(const float* positions, size_t count, const uint32_t* colour, uint32_t width,
                                  uint32_t height, std::vector<uint32_t>& out) {
    out.assign(count, 0);
    if (!colour || !width || !height) return;
    for (size_t i = 0; i < count; ++i) {
        const float x = positions[2 * i], y = positions[2 * i + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        // Pixel n covers n .. n + 1.
        const float column = std::floor(x), row = std::floor(y);
        if (column < 0 || row < 0 || column >= float(width) || row >= float(height)) continue;
        out[i] = colour[size_t(row) * width + size_t(column)];
    }
}

}