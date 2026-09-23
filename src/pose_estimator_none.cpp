// Built without ONNX Runtime: the camera can still give the title a picture,
// but nothing reads a body out of it.
#include "pose_estimator.h"

#include <iostream>

namespace sfr {
std::filesystem::path PoseEstimator::default_model() { return {}; }

std::unique_ptr<PoseEstimator> PoseEstimator::open(const std::filesystem::path&) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::cerr << "NATIVE_POSE unavailable=built-without-onnxruntime\n";
    }
    return nullptr;
}
}
