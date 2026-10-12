#include "camera_capture.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

bool convert_camera_yuv420(const CameraYuvPlanes& planes, uint32_t width, uint32_t height, uint32_t rotation,
                           CameraFrame& frame) {
    if (!width || !height || width % 2 || height % 2 || rotation % 90 || rotation >= 360) return false;
    if (planes.y_row < width || !planes.uv_pixel || planes.uv_row < (width / 2 - 1) * planes.uv_pixel + 1)
        return false;
    const uint64_t luma = uint64_t(height - 1) * planes.y_row + width;
    const uint64_t chroma = uint64_t(height / 2 - 1) * planes.uv_row + uint64_t(width / 2 - 1) * planes.uv_pixel + 1;
    if (planes.y.size() < luma || planes.u.size() < chroma || planes.v.size() < chroma) return false;

    const bool sideways = rotation == 90 || rotation == 270;
    const uint32_t out_width = sideways ? height : width, out_height = sideways ? width : height;
    frame.width = out_width;
    frame.height = out_height;
    frame.bgra.assign(size_t(out_width) * out_height * 4, 0);
    uint8_t* const out = frame.bgra.data();
    for (uint32_t oy = 0; oy < out_height; ++oy)
        for (uint32_t ox = 0; ox < out_width; ++ox) {
            // The source pixel that lands here once turned clockwise.
            uint32_t sx = ox, sy = oy;
            if (rotation == 90) { sx = oy; sy = height - 1 - ox; }
            else if (rotation == 180) { sx = width - 1 - ox; sy = height - 1 - oy; }
            else if (rotation == 270) { sx = width - 1 - oy; sy = ox; }
            const uint64_t c = uint64_t(sy / 2) * planes.uv_row + uint64_t(sx / 2) * planes.uv_pixel;
            write_pixel(out + (size_t(oy) * out_width + ox) * 4, planes.y[uint64_t(sy) * planes.y_row + sx],
                        planes.u[c], planes.v[c]);
        }
    return true;
}

uint32_t camera_upright_rotation(uint32_t sensor_orientation, bool front_facing, uint32_t display_rotation) {
    sensor_orientation %= 360;
    display_rotation %= 360;
    return front_facing ? (sensor_orientation + display_rotation) % 360
                        : (sensor_orientation + 360 - display_rotation) % 360;
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

const char* camera_control_name(CameraControl control) {
    switch (control) {
    case CameraControl::zoom: return "zoom";
    case CameraControl::exposure: return "exposure";
    case CameraControl::gain: return "gain";
    case CameraControl::count: break;
    }
    return "";
}

std::vector<SavedCameraControl> parse_camera_controls(const std::string& text) {
    std::vector<SavedCameraControl> saved;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (second == std::string::npos || first == 0) continue;
        const std::string name = line.substr(first + 1, second - first - 1), value = line.substr(second + 1);
        SavedCameraControl setting;
        setting.device = line.substr(0, first);
        bool known = false;
        for (uint8_t c = 0; c < uint8_t(CameraControl::count); ++c)
            if (name == camera_control_name(CameraControl(c))) {
                setting.control = CameraControl(c);
                known = true;
            }
        if (!known || value.empty()) continue;
        if (value == "auto") {
            setting.value.automatic = true;
        } else {
            char* rest = nullptr;
            const long number = std::strtol(value.c_str(), &rest, 10);
            if (!rest || *rest) continue;
            setting.value = {number, false};
        }
        remember_camera_control(saved, setting);
    }
    return saved;
}

std::string format_camera_controls(const std::vector<SavedCameraControl>& saved) {
    std::string text;
    for (const SavedCameraControl& setting : saved) {
        if (setting.device.empty() || setting.device.find_first_of("\t\r\n") != std::string::npos) continue;
        text += setting.device + '\t' + camera_control_name(setting.control) + '\t' +
                (setting.value.automatic ? std::string("auto") : std::to_string(setting.value.value)) + '\n';
    }
    return text;
}

void remember_camera_control(std::vector<SavedCameraControl>& saved, const SavedCameraControl& setting) {
    for (SavedCameraControl& kept : saved)
        if (kept.device == setting.device && kept.control == setting.control) {
            kept.value = setting.value;
            return;
        }
    saved.push_back(setting);
}
}
