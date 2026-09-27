#pragma once
#include "nui_skeleton.h"
#include "pose_estimator.h"

#include <array>
#include <cstdint>

namespace sfr {

// The twenty joints a NUI skeleton carries, in camera space (metres), made
// from the seventeen points the pose model finds in a picture.
//
// World landmarks retain the model's relative XYZ, centred on the hips at
// pose_distance. The full 3D shoulder span sets one uniform scale. Image-only
// models keep the legacy flat-depth mapping and image shoulder-width scale.
using SkeletonJoints = std::array<std::array<float, 3>, nui_joint_count>;

// False when torso confidence, world coordinates or scale are invalid;
// the joints are left alone. World points must be complete, finite and within
// ten metres of their origin, with a shoulder span of 0.05 to 2 metres.
//
// A camera faces the player, so the player's right hand is on the picture's
// left, and the sensor's +x -- the side the title's cursor centre sits on --
// is that same side. Some cameras hand over a mirrored picture instead (phone
// apps that show you yourself usually do), and nothing in the picture says
// which it is: a mirrored person looks exactly like a person. So it is asked
// for rather than guessed, and the picture is turned back the right way round
// before anything is measured from it.
bool pose_to_joints(const PoseLandmarks& landmarks, uint32_t picture_width, uint32_t picture_height,
                    SkeletonJoints& joints, bool picture_is_mirrored = false);

// The distance the emulated player stands at, and the shoulder half-width the
// mapping fits the picture to (nui_skeleton.cpp's resting pose).
constexpr float pose_distance = 2.5f, pose_shoulder_half_width = 0.18f;

}
