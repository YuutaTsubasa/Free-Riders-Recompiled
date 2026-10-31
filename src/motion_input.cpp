// The device-free half of motion_input.h: which device was chosen, what each
// can do, and a camera's body as a frame's. The devices themselves are
// opened in motion_input_device.cpp.
#include "motion_input.h"

#include <cstdlib>
#include <string_view>

namespace sfr {

const char* motion_device_name(MotionDevice device) {
    switch (device) {
    case MotionDevice::camera: return "camera";
    case MotionDevice::kinect: return "kinect";
    default: return "none";
    }
}

MotionDevice motion_device_from_environment() {
    const char* const text = std::getenv("SFR_CAMERA");
    const std::string_view choice = text ? text : "";
    if (choice.empty() || choice == "off" || choice == "0") return MotionDevice::none;
    if (choice == "kinect") return MotionDevice::kinect;
    return MotionDevice::camera;
}

MotionCapabilities motion_capabilities(MotionDevice device, const KinectSensor* kinect) {
    MotionCapabilities can;
    if (device == MotionDevice::kinect && kinect) {
        can.sensor_bodies = true;
        can.depth_image = kinect->has_depth_stream();
        can.colour_image = kinect->has_colour_stream();
        can.title_smoothing = !kinect_host_smoothing();
        // A v1 tracks two bodies fully, a v2 six.
        can.max_bodies = std::string_view(kinect->model()) == "Kinect v2" ? 6 : 2;
    } else if (device == MotionDevice::camera) {
        can.max_bodies = 1;
    }
    return can;
}

void motion_camera_body(const SkeletonJoints& joints, KinectBody& body) {
    body = KinectBody{};
    body.tracking_id = 1;
    body.sensor_index = 0;
    body.joints = joints;
    body.joint_states.fill(nui_tracked);
    body.position = joints[nui_joint::hip_center];
}

}
