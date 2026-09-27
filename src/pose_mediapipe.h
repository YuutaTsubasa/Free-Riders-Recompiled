#pragma once
#include "pose_estimator.h"
#include <optional>
#include <span>
#include <vector>

namespace sfr {
// Square crop in original picture pixels. Angle rotates crop coordinates
// back to the source image; an upright person has angle zero.
struct PoseRegion { float x=0,y=0,radius=0,angle=0; };
constexpr size_t person_anchor_count=2254;
std::array<float,2> person_anchor(size_t index);
std::optional<PoseRegion> decode_person(std::span<const float> boxes,std::span<const float> scores,
                                      uint32_t width,uint32_t height,float threshold);
// Detector: 224x224 RGB NCHW [-1,1]; pose: 256x256 RGB NHWC [0,1].
std::vector<float> mediapipe_input(const CameraFrame&, const PoseRegion&, bool detector);
bool decode_mediapipe(std::span<const float> image,std::span<const float> world,float confidence,
                      const PoseRegion&,float threshold,PoseLandmarks&,std::optional<PoseRegion>& next);
}
