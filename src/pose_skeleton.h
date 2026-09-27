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
// moves across and up and down. The picture is scaled so that the torso
// (shoulders to hips) comes out as long as the emulated player's, which keeps
// the title's gesture detectors in the range they were tuned against by the
// pad emulation. The torso and not the shoulders: a board is ridden side-on,
// and seen from the front a side-on rider's shoulders are one behind the
// other, while the torso is as long whichever way the body faces or leans
// sideways. For a player facing the camera the two agree, people's torsos
// being about a third longer than their shoulders are wide.

// False when the model was not confident enough about the torso for the rest
// to mean anything, and the joints are left alone.
//
// A camera faces the player, so the player's right hand is on the picture's
// left, and the sensor's +x -- the side the title's cursor centre sits on --
// is that same side. Some cameras hand over a mirrored picture instead (phone
// apps that show you yourself usually do), and nothing in the picture says
// which it is: a mirrored person looks exactly like a person. So it is asked
// for rather than guessed, and the picture is turned back the right way round
// before anything is measured from it.
//
// scale, when given, carries the metres per pixel from one picture to the
// next and follows the torso slowly (most of the way in about 0.7 s at the
// camera's 30 pictures a second), so a crouch or a bend does not make the
// player grow for a moment; zero starts it afresh.
bool pose_to_joints(const PoseLandmarks& landmarks, uint32_t picture_width, uint32_t picture_height,
                    SkeletonJoints& joints, bool picture_is_mirrored = false, float* scale = nullptr);

// The distance the emulated player stands at, the shoulder half-width of its
// resting pose, and its torso from the shoulders' middle to the hips' middle
// (nui_skeleton.cpp), which the mapping fits the picture to.
constexpr float pose_distance = 2.5f, pose_shoulder_half_width = 0.18f, pose_torso_length = 0.48f;

}
