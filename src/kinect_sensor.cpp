#include "kinect_sensor.h"

#include <algorithm>
#include <cmath>

namespace sfr {

KinectSensor::~KinectSensor() = default;

float kinect_level(KinectFrame& frame) {
    const auto& g = frame.gravity;
    const float length = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (!(length > 0.5f)) return 0.0f;
    const std::array<float, 3> up = {g[0] / length, g[1] / length, g[2] / length};
    // cos 30 degrees: a sensor tilted further, or gravity the other way up,
    // is not a tilt to take out.
    if (up[1] < 0.8660254f) return 0.0f;
    // The turn that takes up onto +y: about k = up x (0, 1, 0), by the angle
    // between them (Rodrigues).
    const std::array<float, 3> axis = {-up[2], 0.0f, up[0]};
    const float sine = std::sqrt(axis[0] * axis[0] + axis[2] * axis[2]), cosine = up[1];
    frame.gravity = {0.0f, 1.0f, 0.0f};
    if (sine < 1e-6f) return 0.0f;
    const std::array<float, 3> k = {axis[0] / sine, 0.0f, axis[2] / sine};
    const auto turn = [&](const std::array<float, 3>& v) -> std::array<float, 3> {
        const float dot = k[0] * v[0] + k[2] * v[2];
        const std::array<float, 3> cross = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2],
                                            k[0] * v[1] - k[1] * v[0]};
        std::array<float, 3> out{};
        for (int i = 0; i < 3; ++i) out[i] = v[i] * cosine + cross[i] * sine + k[i] * dot * (1.0f - cosine);
        return out;
    };
    for (KinectBody& body : frame.bodies) {
        for (auto& joint : body.joints) joint = turn(joint);
        body.position = turn(body.position);
    }
    // A plane through turned points keeps its distance; only its normal turns.
    if (frame.floor_plane[0] != 0.0f || frame.floor_plane[1] != 0.0f || frame.floor_plane[2] != 0.0f) {
        const auto normal = turn({frame.floor_plane[0], frame.floor_plane[1], frame.floor_plane[2]});
        frame.floor_plane = {normal[0], normal[1], normal[2], frame.floor_plane[3]};
    }
    return std::atan2(sine, cosine) * 180.0f / 3.14159265358979f;
}

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
    entered_ = {};
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
                ++entries_[slot];
                entered_[slot] = true;
                break;
            }
    }
    return chosen;
}

}
