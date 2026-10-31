#pragma once
#include "kinect_sensor.h"
#include "pose_estimator.h"
#include "pose_skeleton.h"

#include <array>
#include <cstdint>
#include <functional>
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
// A Kinect v2 through libfreenect2 is made the same way on its own depth
// camera (512x424, its factory lens), the colour picture's pixels placed in
// it through the map libfreenect2's registration gives
// (KinectColourDepthGrid).
//
// None of this is a platform's, so it is tested everywhere
// (kinect_depth_body_test).

constexpr uint32_t kinect_depth_width = 320, kinect_depth_height = 240;
constexpr uint32_t kinect_colour_width = 640, kinect_colour_height = 480;

// One depth picture in millimetres (0 for nothing measured), rows top-down:
// a v1's 320x240, as the game's depth image carries it before the player
// index, or another camera's (KinectDepthCamera).
using KinectDepthMillimetres = std::vector<uint16_t>;

// A depth camera without distortion: its picture's size, and the lens that
// puts pixel (x, y) at depth z at ((x - centre_x) z / focal_x,
// (centre_y - y) z / focal_y, z) in camera space. The v1's is the runtime's
// nominal one at 320x240.
struct KinectDepthCamera {
    uint32_t width = kinect_depth_width, height = kinect_depth_height;
    float focal_x = 285.63f, focal_y = 285.63f;
    float centre_x = 160.0f, centre_y = 120.0f;
    size_t pixels() const { return size_t(width) * height; }
};

// A depth pixel at a depth, and back; a colour pixel's place in the depth
// picture (the same direction, the depth camera's wider lens).
std::array<float, 3> kinect_depth_pixel_point(float x, float y, float metres);
std::array<float, 3> kinect_depth_pixel_point(const KinectDepthCamera& camera, float x, float y, float metres);
std::array<float, 2> kinect_colour_to_depth_pixel(float x, float y);

// The depth a colour pixel sees, from the depth picture's pixels around its
// place (radius pixels each way) that lie within near..far metres: their
// median, or 0 when none does.
float kinect_depth_at(const KinectDepthMillimetres& depth, float depth_x, float depth_y, int radius, float near,
                      float far);
float kinect_depth_at(const KinectDepthMillimetres& depth, const KinectDepthCamera& camera, float depth_x,
                      float depth_y, int radius, float near, float far);

// Where colour pixels fall in the depth picture, from the other way round:
// for each depth pixel, the colour pixel it falls on (libfreenect2's
// Registration::apply color_depth_map; -1 for none). The colour picture is
// cut into cells of cell pixels; each cell keeps the nearest depth pixel
// that falls in it, so a hand in front of the wall wins the hand's cells.
// flip turns both pictures round (as a sensor handing them over unmirrored
// is turned, SFR_FREENECT_MIRROR): the map is then of the turned pictures.
class KinectColourDepthGrid {
public:
    void build(const int* colour_of_depth, const KinectDepthMillimetres& depth, const KinectDepthCamera& camera,
               uint32_t colour_width, uint32_t colour_height, uint32_t cell, bool flip);
    // The depth pixel colour pixel (x, y) sees: its cell's, or the nearest
    // one of the cells around (up to three cells away). False when none is.
    bool find(float x, float y, float& depth_x, float& depth_y) const;
private:
    uint32_t columns_ = 0, rows_ = 0, cell_ = 1, depth_width_ = 0;
    std::vector<int32_t> nearest_;     // per cell: a depth pixel, or -1
    std::vector<uint16_t> millimetres_;
};

// Every depth pixel as a camera-space point through the camera's lens
// (three floats a pixel; zeros for a pixel without a depth), as
// kinect_v2_synthesize_depth takes a v2's points.
void kinect_depth_camera_points(const KinectDepthMillimetres& depth, const KinectDepthCamera& camera,
                                std::vector<float>& points);

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
// The same on another depth camera: to_depth places a landmark's colour
// pixel in the depth picture (false for one that falls outside it).
using KinectColourToDepth = std::function<bool(float x, float y, float& depth_x, float& depth_y)>;
bool kinect_depth_body(const PoseLandmarks& landmarks, const KinectDepthMillimetres& depth,
                       const KinectDepthCamera& camera, const KinectColourToDepth& to_depth,
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
uint32_t kinect_cut_player(const KinectDepthMillimetres& depth, const KinectDepthCamera& camera,
                           const SkeletonJoints& joints, uint8_t player, const KinectPlayerCutSettings& settings,
                           std::vector<uint8_t>& index);

// The game's 16-bit depth-and-player image (millimetres << 3 | index), as
// KinectImage pixels (two bytes a pixel, host order).
void kinect_depth_and_player(const KinectDepthMillimetres& depth, const std::vector<uint8_t>& index,
                             std::vector<uint8_t>& pixels);

// A 640x480 depth picture (libfreenect's FREENECT_DEPTH_MM) down to 320x240:
// the nearest measured depth of each 2x2.
void kinect_depth_halve(const uint16_t* depth_640, KinectDepthMillimetres& depth);

}
