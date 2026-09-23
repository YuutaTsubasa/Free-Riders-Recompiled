#include "camera_capture.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace sfr {
namespace {
// BT.601 studio range, which is what a webcam delivers.
uint8_t clamp_byte(int value) { return uint8_t(value < 0 ? 0 : value > 255 ? 255 : value); }

void write_pixel(uint8_t* out, int y, int u, int v) {
    const int c = (y - 16) * 298, d = u - 128, e = v - 128;
    out[0] = clamp_byte((c + 516 * d + 128) >> 8);              // blue
    out[1] = clamp_byte((c - 100 * d - 208 * e + 128) >> 8);    // green
    out[2] = clamp_byte((c + 409 * e + 128) >> 8);              // red
    out[3] = 255;
}
}

bool convert_camera_pixels(CameraPixels format, std::span<const uint8_t> source, uint32_t width, uint32_t height,
                           uint32_t stride, CameraFrame& frame) {
    if (!width || !height || width % 2 || height % 2) return false;
    const uint32_t packed = format == CameraPixels::bgra ? width * 4
                          : format == CameraPixels::yuy2 ? width * 2
                                                         : width;
    if (!stride) stride = packed;
    if (stride < packed) return false;
    const uint64_t needed = format == CameraPixels::nv12
                                ? uint64_t(stride) * height + uint64_t(stride) * (height / 2)
                                : uint64_t(stride) * height;
    if (source.size() < needed) return false;

    frame.width = width;
    frame.height = height;
    frame.bgra.assign(size_t(width) * height * 4, 0);
    uint8_t* const out = frame.bgra.data();
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* const line = source.data() + uint64_t(row) * stride;
        uint8_t* const target = out + uint64_t(row) * width * 4;
        switch (format) {
        case CameraPixels::bgra:
            std::memcpy(target, line, size_t(width) * 4);
            for (uint32_t x = 0; x < width; ++x) target[x * 4 + 3] = 255;
            break;
        case CameraPixels::yuy2:
            // Y0 U Y1 V, one chroma pair for two pixels.
            for (uint32_t x = 0; x < width; x += 2) {
                const int u = line[x * 2 + 1], v = line[x * 2 + 3];
                write_pixel(target + x * 4, line[x * 2], u, v);
                write_pixel(target + (x + 1) * 4, line[x * 2 + 2], u, v);
            }
            break;
        case CameraPixels::nv12: {
            // A plane of Y, then one of interleaved U and V at half size.
            const uint8_t* const chroma =
                source.data() + uint64_t(stride) * height + uint64_t(row / 2) * stride;
            for (uint32_t x = 0; x < width; ++x)
                write_pixel(target + x * 4, line[x], chroma[(x / 2) * 2], chroma[(x / 2) * 2 + 1]);
            break;
        }
        }
    }
    return true;
}

CameraCapture::~CameraCapture() = default;

size_t camera_device_index(const std::vector<std::string>& names, const std::string& wanted) {
    if (names.empty() || wanted.empty()) return 0;
    // Digits are a place in the list and nothing else: a name carries them
    // too ("Live Gamer Portable 2"), and an old setting of 2 meant the third
    // camera, not that one.
    if (std::all_of(wanted.begin(), wanted.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const unsigned long place = std::strtoul(wanted.c_str(), nullptr, 10);
        return place < names.size() ? size_t(place) : 0;
    }
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == wanted) return i;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i].find(wanted) != std::string::npos) return i;
    return 0;
}

size_t chosen_camera_device(const std::vector<std::string>& names, const std::string& wanted) {
    if (!wanted.empty()) return camera_device_index(names, wanted);
    const char* const text = std::getenv("SFR_CAMERA_DEVICE");
    return camera_device_index(names, text ? text : "");
}
}
