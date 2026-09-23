#pragma once
#include "nui_skeleton.h"
#include "pose_estimator.h"

#include <array>
#include <cstdint>

namespace sfr {

// The twenty joints a NUI skeleton carries, in camera space (metres), made
// from the seventeen points the pose model finds in a picture.
//
// A webcam gives no depth, so every joint stands at one distance and only
// moves across and up and down. The picture is scaled so that the shoulders
// come out as wide as the emulated player's, which keeps the title's gesture
// detectors in the range they were tuned against by the pad emulation.
using SkeletonJoints = std::array<std::array<float, 3>, nui_joint_count>;

// False when the model was not confident enough about the torso for the rest
// to mean anything, and the joints are left alone.
bool pose_to_joints(const PoseLandmarks& landmarks, uint32_t picture_width, uint32_t picture_height,
                    SkeletonJoints& joints);

// The distance the emulated player stands at, and the shoulder half-width the
// mapping fits the picture to (nui_skeleton.cpp's resting pose).
constexpr float pose_distance = 2.5f, pose_shoulder_half_width = 0.18f;

}
