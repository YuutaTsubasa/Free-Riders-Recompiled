#pragma once
#include "kinect_sensor.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sfr {

class CameraPlayer;

// Free Riders' motion input: every device that tracks a body for the title's
// Kinect, behind one door (docs/motion-input.md).
//
//   device backends                 one kind of data          game input mapping
//   camera + pose model --------\                         /-> sensor bodies: written as the
//     (Windows, Linux, macOS,    \                       /     console's sensor wrote them; the
//      Android, iOS)              >-- MotionFrame -------<      title's detectors read them
//   Kinect v1 / v2 -------------/   + capabilities       \-> one body over the first player's
//     (Windows SDK, macOS                                      pad-made one; the race reads it
//      libfreenect[2])                                         through the camera bridge
//
// A device is only ever the left column: it opens, it hands over frames, it
// says what it can do (MotionCapabilities). The game's side
// (nui_hooks.cpp, nui_race_hooks.cpp) asks for capabilities, never for the
// device, so a new device is a new backend and nothing else.
enum class MotionDevice : uint8_t { none, camera, kinect };
const char* motion_device_name(MotionDevice device);
// The launcher's choice (SFR_CAMERA): "kinect" a Kinect, nothing, "off" or
// "0" none, anything else ("motion", the old "picture") the camera.
MotionDevice motion_device_from_environment();

// What a device gives the game, which decides how the game reads it.
struct MotionCapabilities {
    // The bodies are the device's own: each keeps its tracking id and slot,
    // there may be several, and they are handed to the title as a console's
    // sensor handed them (identity, two players, the Pause Gesture each).
    // False: one body, standing in for the first player over the pad's
    // emulated one while nobody touches the pad (camera_input.h).
    bool sensor_bodies = false;
    // A depth and player index image for the title's own image stream, which
    // its race detectors read. Without one, the race takes the first
    // player's body through the camera bridge (camera_race_motion.h).
    bool depth_image = false;
    bool colour_image = false;
    // The title's NuiTransformSmooth smooths the bodies (nui_smooth_hooks.cpp)
    // rather than the device: a camera smooths its own (pose_smoothing.h).
    bool title_smoothing = false;
    // How many bodies a frame may carry.
    uint32_t max_bodies = 0;
};
MotionCapabilities motion_capabilities(MotionDevice device, const KinectSensor* kinect);

// A frame of bodies is a Kinect's frame whatever the device: the NUI's
// skeleton space (metres, +x the player's right, +z away from the device),
// its twenty joints with their states.
using MotionFrame = KinectFrame;

// How fresh the device's tracking is, for choosing between it and the pad.
struct MotionStatus {
    bool detected = false;            // a body in the device's newest look
    double observation_age_ms = -1;   // since that look; -1 none yet
    double pose_age_ms = -1;          // since the newest body; -1 none yet
};

// The camera's body as a frame's one body: tracking id 1 in slot 0, every
// joint tracked (the camera's pose has been steadied and smoothed already),
// its centre at the hips.
void motion_camera_body(const SkeletonJoints& joints, KinectBody& body);

class MotionInput {
public:
    // The one the game reads.
    static MotionInput& instance();
    // The device chosen, whether or not it has been opened.
    MotionDevice device() const { return device_; }
    // Opens the chosen device, once; from any thread, and every caller
    // returns after it has. Logs NATIVE_KINECT started=... for a Kinect, as
    // before (the camera logs NATIVE_CAMERA_PLAYER itself).
    void start();
    // Whether a device is open and handing over frames, and what it can do
    // (nothing before start() has returned, or with no device): read on any
    // thread, as often as wanted.
    bool running() const { return capabilities().max_bodies != 0; }
    MotionCapabilities capabilities() const {
        return opened_.load(std::memory_order_acquire) ? capabilities_ : MotionCapabilities{};
    }
    // The newest frame, if one has arrived since the last call; false leaves
    // the frame alone. The status is sampled either way.
    bool next(MotionFrame& frame, MotionStatus* status = nullptr);
    // Whether the title is in its menus, where a camera steadies the hands
    // that point (a Kinect's are steadied by the game, kinect_stability.h).
    void set_in_menu(bool in_menu);
    // The device itself, for what only it has: its adjustment window, its
    // images, its tilt motor. Null when it is not that device or not open.
    KinectSensor* kinect() const { return opened_.load(std::memory_order_acquire) ? kinect_.get() : nullptr; }
    CameraPlayer* camera() const { return opened_.load(std::memory_order_acquire) ? camera_.get() : nullptr; }
    ~MotionInput();

private:
    explicit MotionInput(MotionDevice device);
    MotionDevice device_;
    std::once_flag started_;
    std::unique_ptr<KinectSensor> kinect_;
    std::unique_ptr<CameraPlayer> camera_;
    uint64_t camera_frames_ = 0;
    MotionCapabilities capabilities_;  // written once, before opened_
    std::atomic<bool> opened_{false};
};

}
