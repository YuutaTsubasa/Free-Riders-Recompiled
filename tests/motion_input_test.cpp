// motion_input.h: the device chosen from the launcher's setting, what each
// device can do (which decides how the game reads it), and a camera's body
// as a frame's.
#include "motion_input.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void set_camera(const char* value) {
#ifdef _WIN32
    _putenv_s("SFR_CAMERA", value ? value : "");
#else
    if (value) setenv("SFR_CAMERA", value, 1);
    else unsetenv("SFR_CAMERA");
#endif
}

void choice() {
    using sfr::MotionDevice;
    set_camera(nullptr);
    require(sfr::motion_device_from_environment() == MotionDevice::none, "no setting is no device");
    set_camera("off");
    require(sfr::motion_device_from_environment() == MotionDevice::none, "off is no device");
    set_camera("0");
    require(sfr::motion_device_from_environment() == MotionDevice::none, "0 is no device");
    set_camera("kinect");
    require(sfr::motion_device_from_environment() == MotionDevice::kinect, "kinect is the Kinect");
    set_camera("motion");
    require(sfr::motion_device_from_environment() == MotionDevice::camera, "motion is the camera");
    set_camera("picture");
    require(sfr::motion_device_from_environment() == MotionDevice::camera, "the old picture setting still opens the camera");
    set_camera(nullptr);
    require(std::string(sfr::motion_device_name(MotionDevice::kinect)) == "kinect", "names for the log");
}

struct FakeKinect : sfr::KinectSensor {
    const char* name;
    bool depth;
    FakeKinect(const char* name, bool depth) : name(name), depth(depth) {}
    bool has_depth_stream() const override { return depth; }
    bool next(sfr::KinectFrame&) override { return false; }
    const char* model() const override { return name; }
};

void capabilities() {
    using sfr::MotionDevice;
    const FakeKinect v1("Kinect v1", true), v2("Kinect v2", false);
    const auto one = sfr::motion_capabilities(MotionDevice::kinect, &v1);
    require(one.sensor_bodies && one.depth_image && one.max_bodies == 2, "a v1's bodies are the sensor's, two of them, with depth");
    require(one.title_smoothing, "the title smooths a sensor's bodies unless the backend does");
    const auto two = sfr::motion_capabilities(MotionDevice::kinect, &v2);
    require(two.sensor_bodies && !two.depth_image && two.max_bodies == 6,
            "a v2 tracks six; a depth stream that would not open is not claimed");
    const auto camera = sfr::motion_capabilities(MotionDevice::camera, nullptr);
    require(!camera.sensor_bodies && !camera.depth_image && !camera.title_smoothing && camera.max_bodies == 1,
            "a camera is one body over the first player, no depth, smoothed by itself");
    require(sfr::motion_capabilities(MotionDevice::kinect, nullptr).max_bodies == 0, "a Kinect that did not open gives nothing");
    require(sfr::motion_capabilities(MotionDevice::none, nullptr).max_bodies == 0, "no device gives nothing");
}

void camera_body() {
    sfr::SkeletonJoints joints{};
    for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) joints[j] = {0.01f * float(j), 0.5f, 2.5f};
    sfr::KinectBody body;
    body.tracking_id = 9;
    body.hand_states = {3, 3};
    sfr::motion_camera_body(joints, body);
    require(body.tracking_id == 1 && body.sensor_index == 0, "the camera's one body is tracking id 1 in slot 0");
    require(body.joints == joints, "its joints as the camera made them");
    for (uint32_t state : body.joint_states) require(state == sfr::nui_tracked, "every joint tracked");
    require(body.position == joints[sfr::nui_joint::hip_center], "its centre at the hips");
    require(body.hand_states[0] == 0 && body.hand_states[1] == 0 && body.clipped_edges == 0,
            "nothing a camera does not know is left from before");
}
}

int main() {
    try {
        choice();
        capabilities();
        camera_body();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Motion input checks passed\n";
}
