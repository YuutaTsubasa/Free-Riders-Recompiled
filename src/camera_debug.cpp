#include "camera_debug.h"
#include <algorithm>
#include <cmath>
namespace sfr {
CameraDebugState camera_debug_state(const CameraDebugFrame& frame, CameraDebugClock::time_point now) {
    if (!frame.game_frame) return CameraDebugState::waiting;
    const double age = std::max(0.0, std::chrono::duration<double, std::milli>(now-frame.submitted).count());
    if (age > 500) return CameraDebugState::stale;
    if (frame.controller_active) return CameraDebugState::controller;
    if (!frame.camera_available) return CameraDebugState::unavailable;
    if (!frame.has_pose) return CameraDebugState::waiting;
    if (frame.detected && frame.observation_age_ms >= 0 && frame.observation_age_ms + age <= 500)
        return CameraDebugState::live;
    return CameraDebugState::held;
}
CameraDebugPoint camera_debug_project(const std::array<float,3>& joint, CameraDebugView view, float width, float height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return {};
    for (const float value : joint) if (!std::isfinite(value) || std::abs(value) > 100) return {};
    const float scale = std::min(width,height)/2.5f;
    const float x = width/2 + (view == CameraDebugView::front ? joint[0] : joint[2]-pose_distance)*scale;
    const float y = height/2 - joint[1]*scale;
    return {x,y,x >= 0 && x <= width && y >= 0 && y <= height};
}
std::span<const CameraDebugBone> camera_debug_bones() {
    using namespace nui_joint;
    static constexpr CameraDebugBone bones[] = {
        {hip_center,spine,0},{spine,shoulder_center,0},{shoulder_center,head,0},
        {shoulder_center,shoulder_left,-1},{shoulder_left,elbow_left,-1},{elbow_left,wrist_left,-1},{wrist_left,hand_left,-1},
        {shoulder_center,shoulder_right,1},{shoulder_right,elbow_right,1},{elbow_right,wrist_right,1},{wrist_right,hand_right,1},
        {hip_center,hip_left,-1},{hip_left,knee_left,-1},{knee_left,ankle_left,-1},{ankle_left,foot_left,-1},
        {hip_center,hip_right,1},{hip_right,knee_right,1},{knee_right,ankle_right,1},{ankle_right,foot_right,1},
    };
    return bones;
}
}
