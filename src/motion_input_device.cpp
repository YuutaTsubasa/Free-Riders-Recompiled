// The devices behind motion_input.h: opened once, on whichever thread asks
// first (the title may open its image streams before its first skeleton).
#include "motion_input.h"

#include "camera_player.h"

#include <chrono>
#include <iostream>
#include <string>

namespace sfr {

MotionInput::MotionInput(MotionDevice device) : device_(device) {}
MotionInput::~MotionInput() = default;

MotionInput& MotionInput::instance() {
    // Never destroyed: the game may end on any thread, with a device's
    // worker still running.
    static MotionInput* const input = new MotionInput(motion_device_from_environment());
    return *input;
}

void MotionInput::start() {
    std::call_once(started_, [this] {
        if (device_ == MotionDevice::kinect) {
            std::string why;
            kinect_ = KinectSensor::open_selected(kinect_selected_model(), &why);
            std::cerr << "NATIVE_KINECT started=" << (kinect_ ? 1 : 0);
            if (kinect_) std::cerr << " model=" << kinect_->model() << " depth=" << kinect_->has_depth_stream();
            else std::cerr << " reason=" << why << " fallback=pad";
            std::cerr << '\n';
            // No substitute: without a depth stream the game gets none either
            // (NuiImageStreamOpen is left to the library, which fails without
            // a sensor) and its own detectors see nothing.
            if (kinect_ && !kinect_->has_depth_stream())
                std::cerr << "NATIVE_KINECT_DEPTH unavailable model=" << kinect_->model() << " reason="
                          << (kinect_->depth_failure().empty() ? "unknown" : kinect_->depth_failure()) << '\n';
        } else if (device_ == MotionDevice::camera) {
            camera_ = CameraPlayer::start();
        }
        capabilities_ = motion_capabilities(device_, kinect_.get());
        if (device_ == MotionDevice::camera && !camera_) capabilities_ = {};
        opened_.store(true, std::memory_order_release);
        const MotionCapabilities& can = capabilities_;
        std::cerr << "MOTION_INPUT device=" << motion_device_name(device_) << " running=" << running()
                  << " sensor_bodies=" << can.sensor_bodies << " depth=" << can.depth_image
                  << " max_bodies=" << can.max_bodies << '\n';
    });
}

bool MotionInput::next(MotionFrame& frame, MotionStatus* status) {
    const auto now = std::chrono::steady_clock::now();
    if (kinect_) {
        const bool fresh = kinect_->next(frame);
        if (status) {
            status->detected = !frame.bodies.empty();
            const bool seen = frame.number && frame.captured_at != std::chrono::steady_clock::time_point{};
            status->observation_age_ms = seen ? std::chrono::duration<double, std::milli>(now - frame.captured_at).count() : -1;
            status->pose_age_ms = seen && !frame.bodies.empty() ? status->observation_age_ms : -1;
        }
        return fresh;
    }
    if (camera_) {
        SkeletonJoints joints{};
        CameraTrackingStatus tracking;
        const bool fresh = camera_->joints(joints, &tracking);
        if (status) {
            status->detected = tracking.detected;
            status->observation_age_ms = tracking.observation_age_ms;
            status->pose_age_ms = tracking.pose_age_ms;
        }
        if (!fresh) return false;
        frame.number = ++camera_frames_;
        frame.captured_at = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double, std::milli>(tracking.pose_age_ms > 0 ? tracking.pose_age_ms : 0));
        frame.floor_plane = {};
        frame.gravity = {};
        frame.bodies.resize(1);
        motion_camera_body(joints, frame.bodies.front());
        return true;
    }
    return false;
}

void MotionInput::set_in_menu(bool in_menu) {
    if (camera_) camera_->set_in_menu(in_menu);
}

}
