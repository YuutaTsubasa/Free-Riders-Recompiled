#pragma once
#include "kinect_sensor.h"
#include "pose_estimator.h"
#include "pose_skeleton.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sfr {

// What the Kinect for Windows runtime makes from a Kinect v1's cameras, made
// here instead on a host that has only the cameras (macOS through
// libfreenect, #305): a skeleton and the depth image's player index.
//
// The skeleton comes from the colour picture (the camera's pose model,
// pose_estimator.h, which names the body's sides itself) placed in metres by
// the depth: the hips where the torso's depth puts them, each joint at its
// own depth when that agrees with the model's 3D shape (a thin arm's sample
// often lands on the wall behind it), the shape's place otherwise. The
// player index is grown through the depth image from the skeleton's pixels,
// across neighbours of nearly the same depth, kept to the skeleton's
// surroundings and off the floor.
//
// Space and pictures are the runtime's: camera space in metres with +x to
// the player's right and +y up (kinect_sensor.h), the pictures as a Kinect
// v1 hands them over through the runtime, mirrored like a mirror (the
// player's right hand on the picture's right). The lens is the runtime's
// nominal one (kinect_preview.h), which leaves out the two cameras'
// 2.5 cm apart: a few colour pixels at the distance players stand.
//
// None of this is a platform's, so it is tested everywhere
// (kinect_depth_body_test).

constexpr uint32_t kinect_depth_width = 320, kinect_depth_height = 240;
constexpr uint32_t kinect_colour_width = 640, kinect_colour_height = 480;

// One 320x240 depth picture in millimetres (0 for nothing measured), rows
// top-down, as the game's depth image carries it before the player index.
using KinectDepthMillimetres = std::vector<uint16_t>;

// A depth pixel at a depth, and back; a colour pixel's place in the depth
// picture (the same direction, the depth camera's wider lens).
std::array<float, 3> kinect_depth_pixel_point(float x, float y, float metres);
std::array<float, 2> kinect_colour_to_depth_pixel(float x, float y);

// The depth a colour pixel sees, from the depth picture's pixels around its
// place (radius pixels each way) that lie within near..far metres: their
// median, or 0 when none does.
float kinect_depth_at(const KinectDepthMillimetres& depth, float depth_x, float depth_y, int radius, float near,
                      float far);

struct KinectDepthBodySettings {
    float joint_agreement = 0.25f;  // metres a joint's own depth may differ from the shape's place
    float min_score = 0.3f;         // a landmark less sure than this keeps the shape's place
};

// The skeleton for one body from the model's landmarks (colour pixels, with
// world points) and the depth. False when there is no torso, no world shape,
// or no depth at the torso. joint_states: 2 a joint at its own depth, 1 at
// the shape's place.
bool kinect_depth_body(const PoseLandmarks& landmarks, const KinectDepthMillimetres& depth,
                       const KinectDepthBodySettings& settings, SkeletonJoints& joints,
                       std::array<uint32_t, nui_joint_count>& joint_states);

struct KinectPlayerCutSettings {
    float step_metres = 0.05f;    // the depth a neighbour may differ by and still be the player
    float margin_metres = 0.30f;  // how far beyond the skeleton the player may reach
    float floor_metres = 0.07f;   // how far below the lower ankle the floor starts
};

// Marks the player's pixels (player, 1..6) in index, a 320x240 picture of
// player indices (0 none), grown from the skeleton's joints through the
// depth. Returns how many pixels were marked.
uint32_t kinect_cut_player(const KinectDepthMillimetres& depth, const SkeletonJoints& joints, uint8_t player,
                           const KinectPlayerCutSettings& settings, std::vector<uint8_t>& index);

// The game's 16-bit depth-and-player image (millimetres << 3 | index), as
// KinectImage pixels (two bytes a pixel, host order).
void kinect_depth_and_player(const KinectDepthMillimetres& depth, const std::vector<uint8_t>& index,
                             std::vector<uint8_t>& pixels);

// A 640x480 depth picture (libfreenect's FREENECT_DEPTH_MM) down to 320x240:
// the nearest measured depth of each 2x2.
void kinect_depth_halve(const uint16_t* depth_640, KinectDepthMillimetres& depth);

}
