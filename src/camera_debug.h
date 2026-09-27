#pragma once
#include "pose_skeleton.h"
#include <chrono>
#include <memory>
#include <span>

namespace sfr {
using CameraDebugClock = std::chrono::steady_clock;
// No camera pixels: these are the logical P1 joints just written to the guest,
// from either the camera or the controller.
struct CameraDebugFrame {
    SkeletonJoints joints{};
    uint64_t game_frame = 0;
    bool camera_available = false, has_pose = false, detected = false;
    bool controller_active = false;
    double observation_age_ms = -1, pose_age_ms = -1;
    CameraDebugClock::time_point submitted{};
};
enum class CameraDebugState { waiting, unavailable, live, held, stale, controller };
enum class CameraDebugView { front, side };
struct CameraDebugPoint { float x=0, y=0; bool visible=false; };
struct CameraDebugBone { uint32_t from, to; int side; }; // -1 left, +1 right, 0 centre
CameraDebugState camera_debug_state(const CameraDebugFrame&, CameraDebugClock::time_point now);
CameraDebugPoint camera_debug_project(const std::array<float,3>&, CameraDebugView, float width, float height);
std::span<const CameraDebugBone> camera_debug_bones();

class CameraDebugWindow {
public:
    // Enabled only by SFR_CAMERA_DEBUG=1 together with SFR_CAMERA=motion.
    // Non-Windows builds return null. Failure never stops camera/game input.
    static std::unique_ptr<CameraDebugWindow> start();
    ~CameraDebugWindow();
    void publish(const CameraDebugFrame& frame);
    bool closed() const;
    struct Impl;
private:
    explicit CameraDebugWindow(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
