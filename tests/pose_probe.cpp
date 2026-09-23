// Runs the pose model over the camera (or a BMP written by sfr_camera_probe)
// and prints where it thinks the body is:
//   sfr_pose_probe [picture.bmp]
#include "camera_capture.h"
#include "pose_estimator.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace {
// The 32-bit top-down BMP sfr_camera_probe writes, and nothing else.
bool read_bmp(const char* path, sfr::CameraFrame& frame) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint8_t header[54] = {};
    in.read(reinterpret_cast<char*>(header), sizeof header);
    if (!in || header[0] != 'B' || header[1] != 'M' || header[28] != 32) return false;
    const auto read32 = [&](int at) {
        return uint32_t(header[at]) | uint32_t(header[at + 1]) << 8 | uint32_t(header[at + 2]) << 16 |
               uint32_t(header[at + 3]) << 24;
    };
    frame.width = read32(18);
    const int32_t height = int32_t(read32(22));
    if (height >= 0) return false;  // bottom-up is not what the probe writes
    frame.height = uint32_t(-height);
    frame.bgra.assign(size_t(frame.width) * frame.height * 4, 0);
    in.read(reinterpret_cast<char*>(frame.bgra.data()), std::streamsize(frame.bgra.size()));
    frame.number = 1;
    return bool(in);
}

const char* name_of(uint32_t point) {
    using namespace sfr::pose_point;
    switch (point) {
    case nose: return "nose";
    case eye_left: return "eye_left";
    case eye_right: return "eye_right";
    case ear_left: return "ear_left";
    case ear_right: return "ear_right";
    case shoulder_left: return "shoulder_left";
    case shoulder_right: return "shoulder_right";
    case elbow_left: return "elbow_left";
    case elbow_right: return "elbow_right";
    case wrist_left: return "wrist_left";
    case wrist_right: return "wrist_right";
    case hip_left: return "hip_left";
    case hip_right: return "hip_right";
    case knee_left: return "knee_left";
    case knee_right: return "knee_right";
    case ankle_left: return "ankle_left";
    default: return "ankle_right";
    }
}
}

int main(int argc, char** argv) {
    auto estimator = sfr::PoseEstimator::open(sfr::PoseEstimator::default_model());
    if (!estimator) return 1;

    sfr::CameraFrame frame;
    if (argc > 1) {
        if (!read_bmp(argv[1], frame)) {
            std::cerr << "could not read " << argv[1] << " (a 32-bit top-down BMP is expected)\n";
            return 2;
        }
    } else {
        auto camera = sfr::CameraCapture::open(640, 480);
        if (!camera) return 3;
        for (int attempt = 0; attempt < 500 && !frame.number; ++attempt)
            if (!camera->next(frame)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!frame.number) {
            std::cerr << "no picture arrived\n";
            return 4;
        }
    }

    sfr::PoseLandmarks landmarks{};
    const auto started = std::chrono::steady_clock::now();
    const bool found = estimator->estimate(frame, landmarks);
    const double milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "picture " << frame.width << 'x' << frame.height << " body=" << found << " in " << milliseconds
              << " ms\n";
    for (uint32_t point = 0; point < sfr::pose_point::count; ++point)
        std::cout << "  " << name_of(point) << ' ' << landmarks[point].x << ',' << landmarks[point].y << " score "
                  << landmarks[point].score << '\n';
    return found ? 0 : 5;
}
