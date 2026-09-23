#pragma once
#include "camera_capture.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace sfr {

// The seventeen body points RTMPose gives, in the order COCO numbers them.
namespace pose_point {
constexpr uint32_t nose = 0, eye_left = 1, eye_right = 2, ear_left = 3, ear_right = 4;
constexpr uint32_t shoulder_left = 5, shoulder_right = 6, elbow_left = 7, elbow_right = 8;
constexpr uint32_t wrist_left = 9, wrist_right = 10, hip_left = 11, hip_right = 12;
constexpr uint32_t knee_left = 13, knee_right = 14, ankle_left = 15, ankle_right = 16;
constexpr uint32_t count = 17;
}

// One point, in the camera picture's pixels, with the model's confidence.
struct PoseLandmark {
    float x = 0, y = 0, score = 0;
};
using PoseLandmarks = std::array<PoseLandmark, pose_point::count>;

// Finds a body in a camera picture. One model, one session, one thread: the
// estimate runs where the camera frames are read, not on the game's threads.
class PoseEstimator {
public:
    virtual ~PoseEstimator();
    // False when the model found nothing it is confident about.
    virtual bool estimate(const CameraFrame& frame, PoseLandmarks& landmarks) = 0;
    // Null when the runtime or the model is missing, or the model is not one
    // this understands. SFR_POSE_MODEL overrides the path.
    static std::unique_ptr<PoseEstimator> open(const std::filesystem::path& model);
    // Where the model lives beside the program or in the checkout.
    static std::filesystem::path default_model();
};

// The SimCC decode, which is the part worth testing on its own: each point
// has a row of scores along x and one along y, and its place is the best
// column of each, divided by the split ratio and mapped back through the
// crop the picture was taken with.
struct PoseCrop {
    float origin_x = 0, origin_y = 0;  // where the crop starts in the picture
    float scale_x = 1, scale_y = 1;    // picture pixels per model pixel
};
void decode_simcc(const float* simcc_x, const float* simcc_y, uint32_t points, uint32_t width_bins,
                  uint32_t height_bins, float split_ratio, const PoseCrop& crop, PoseLandmarks& landmarks);

// The crop that maps a picture of this size onto the model's input, keeping
// the aspect ratio (the letterbox RTMPose's affine transform amounts to for
// a whole-picture box).
PoseCrop pose_crop(uint32_t picture_width, uint32_t picture_height, uint32_t model_width, uint32_t model_height);

}
