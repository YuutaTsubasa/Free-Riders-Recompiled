// Opens the host's camera and writes the first picture it delivers, so that
// a machine can be checked without starting the game:
//   sfr_camera_probe [out.bmp] [width] [height]
#include "camera_capture.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <string>
#include <vector>

namespace {
bool write_bmp(const char* path, const sfr::CameraFrame& frame) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint32_t pixels = frame.width * frame.height * 4, size = 54 + pixels;
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    const auto put32 = [&](int at, uint32_t value) {
        header[at] = uint8_t(value); header[at + 1] = uint8_t(value >> 8);
        header[at + 2] = uint8_t(value >> 16); header[at + 3] = uint8_t(value >> 24);
    };
    put32(2, size); put32(10, 54); put32(14, 40);
    put32(18, frame.width);
    put32(22, uint32_t(-int32_t(frame.height)));  // negative: rows top-down
    header[26] = 1; header[28] = 32;
    put32(34, pixels);
    out.write(reinterpret_cast<const char*>(header), sizeof header);
    out.write(reinterpret_cast<const char*>(frame.bgra.data()), pixels);
    return bool(out);
}
}

int main(int argc, char** argv) {
    // --list names the cameras the host offers, in the order SFR_CAMERA_DEVICE
    // numbers them (the launcher's camera setting lists the same).
    if (argc > 1 && std::string(argv[1]) == "--list") {
        const auto cameras = sfr::CameraCapture::devices();
        std::cout << cameras.size() << " camera(s)\n";
        for (size_t i = 0; i < cameras.size(); ++i) std::cout << "  " << i << ' ' << cameras[i] << '\n';
        return cameras.empty() ? 1 : 0;
    }
    const char* path = argc > 1 ? argv[1] : "camera.bmp";
    const uint32_t width = argc > 2 ? uint32_t(std::strtoul(argv[2], nullptr, 10)) : 640;
    const uint32_t height = argc > 3 ? uint32_t(std::strtoul(argv[3], nullptr, 10)) : 480;
    auto camera = sfr::CameraCapture::open(width, height);
    if (!camera) {
        std::cerr << "no camera\n";
        return 1;
    }
    sfr::CameraFrame frame;
    // A camera takes a moment to start; five seconds is more than enough.
    for (int attempt = 0; attempt < 500 && !frame.number; ++attempt) {
        if (!camera->next(frame)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!frame.number) {
        std::cerr << "no picture arrived\n";
        return 2;
    }
    if (!write_bmp(path, frame)) {
        std::cerr << "could not write " << path << '\n';
        return 3;
    }
    std::cout << "wrote " << path << ' ' << frame.width << 'x' << frame.height << '\n';
    return 0;
}
