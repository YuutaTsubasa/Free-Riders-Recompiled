#include "kinect_sensor.h"

#include <algorithm>

namespace sfr {

KinectSensor::~KinectSensor() = default;

std::string kinect_open_failure(const std::string& v1, const std::string& v2) {
    const auto installed = [](const std::string& why) { return !why.empty() && why != "no-runtime"; };
    if (installed(v1) && installed(v2)) return v1 == "no-sensor" ? v2 : v1;  // one of them saw more
    if (installed(v1)) return v1;
    if (installed(v2)) return v2;
    return "no-runtime";
}

void kinect_v2_body(const std::array<KinectV2Joint, kinect_v2_joint_count>& joints, uint32_t tracking_id,
                    KinectBody& body) {
    body.tracking_id = tracking_id;
    for (uint32_t j = 0; j < nui_joint_count; ++j) {
        // v1's shoulder centre is v2's spine-shoulder, not its neck.
        const KinectV2Joint& joint = joints[j == nui_joint::shoulder_center ? kinect_v2_joint::spine_shoulder : j];
        body.joints[j] = joint.position;
        body.joint_states[j] = joint.state > nui_tracked ? nui_tracked : joint.state;
    }
    // v2 reports no centre of its own; v1's sits near the hips.
    body.position = joints[kinect_v2_joint::spine_base].position;
}

std::array<const KinectBody*, KinectPlayerSlots::players> KinectPlayerSlots::assign(const KinectFrame& frame) {
    std::array<const KinectBody*, players> chosen{};
    const auto find = [&](uint32_t id) -> const KinectBody* {
        for (const auto& body : frame.bodies)
            if (body.tracking_id == id) return &body;
        return nullptr;
    };
    // A body the sensor still tracks keeps its slot; a lost one frees it.
    for (uint32_t slot = 0; slot < players; ++slot) {
        chosen[slot] = tracking_[slot] ? find(tracking_[slot]) : nullptr;
        if (!chosen[slot]) tracking_[slot] = 0;
    }
    // Bodies not yet playing take the free slots, the first player's first.
    for (const auto& body : frame.bodies) {
        if (!body.tracking_id) continue;
        if (std::find(tracking_.begin(), tracking_.end(), body.tracking_id) != tracking_.end()) continue;
        for (uint32_t slot = 0; slot < players; ++slot)
            if (!tracking_[slot]) {
                tracking_[slot] = body.tracking_id;
                chosen[slot] = &body;
                break;
            }
    }
    return chosen;
}

}
