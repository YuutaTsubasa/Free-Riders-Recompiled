#include "kinect_sensor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace sfr {

KinectSensor::~KinectSensor() = default;

std::string kinect_edges_text(uint8_t edges) {
    std::string text;
    const auto add = [&](uint8_t bit, const char* name) {
        if (!(edges & bit)) return;
        if (!text.empty()) text += '+';
        text += name;
    };
    add(kinect_edge::top, "top");
    add(kinect_edge::bottom, "bottom");
    add(kinect_edge::left, "left");
    add(kinect_edge::right, "right");
    return text.empty() ? "none" : text;
}

uint8_t kinect_edges_move(uint8_t edges) {
    uint8_t move = 0;
    const bool top = edges & kinect_edge::top, bottom = edges & kinect_edge::bottom;
    const bool left = edges & kinect_edge::left, right = edges & kinect_edge::right;
    // Cut at both ends of an axis: the picture is too small for the player.
    if ((top && bottom) || (left && right)) move |= kinect_edge_move::step_back;
    else {
        if (top) move |= kinect_edge_move::tilt_up | kinect_edge_move::step_back;
        if (bottom) move |= kinect_edge_move::tilt_down | kinect_edge_move::step_back;
        // The picture's left is the player's right (the player faces the
        // sensor, the picture is not mirrored): move to the player's own left.
        if (left) move |= kinect_edge_move::player_left;
        if (right) move |= kinect_edge_move::player_right;
    }
    return move;
}

std::string kinect_open_failure(const std::string& v1, const std::string& v2) {
    const auto installed = [](const std::string& why) { return !why.empty() && why != "no-runtime"; };
    if (installed(v1) && installed(v2)) return v1 == "no-sensor" ? v2 : v1;  // one of them saw more
    if (installed(v1)) return v1;
    if (installed(v2)) return v2;
    return "no-runtime";
}

int kinect_selected_model() {
    const char* const value = std::getenv("SFR_KINECT_MODEL");
    return value && std::string_view(value) == "2" ? 2 : 1;
}

bool kinect_host_smoothing() {
    const char* const value = std::getenv("SFR_KINECT_SMOOTH");
    return value && *value == '1';
}

std::unique_ptr<KinectSensor> KinectSensor::open_selected(int model, std::string* why) {
    if (model == 2) return open_model(2, why);
    std::string v1_why, v2_why;
    if (auto sensor = open_model(1, &v1_why)) return sensor;
    if (auto sensor = open_model(2, &v2_why)) {
        std::cerr << "NATIVE_KINECT no Kinect (Xbox 360) opened, using the Kinect v2 found instead\n";
        return sensor;
    }
    if (why) *why = kinect_open_failure(v1_why, v2_why);
    return nullptr;
}

void kinect_v2_body(const std::array<KinectV2Joint, kinect_v2_joint_count>& joints, uint32_t tracking_id,
                    uint32_t sensor_index, KinectBody& body) {
    body.tracking_id = tracking_id;
    body.sensor_index = sensor_index;
    for (uint32_t j = 0; j < nui_joint_count; ++j) {
        // v1's shoulder centre is v2's spine-shoulder, not its neck.
        const KinectV2Joint& joint = joints[j == nui_joint::shoulder_center ? kinect_v2_joint::spine_shoulder : j];
        body.joints[j] = joint.position;
        body.joint_states[j] = joint.state > nui_tracked ? nui_tracked : joint.state;
    }
    // v2 reports no centre of its own; v1's sits near the hips.
    body.position = joints[kinect_v2_joint::spine_base].position;
    // The fingertips and thumbs a v1 does not have (the Stop Sign gesture).
    const uint32_t tips[2] = {kinect_v2_joint::hand_tip_left, kinect_v2_joint::hand_tip_right};
    const uint32_t thumbs[2] = {kinect_v2_joint::thumb_left, kinect_v2_joint::thumb_right};
    for (int side = 0; side < 2; ++side) {
        // Tracked, not inferred: a guessed fingertip gives a guessed palm.
        const bool seen = joints[tips[side]].state == 2 && joints[thumbs[side]].state == 2;
        body.hand_tips[side] = seen ? joints[tips[side]].position : std::array<float, 3>{};
        body.thumbs[side] = seen ? joints[thumbs[side]].position : std::array<float, 3>{};
    }
}

bool kinect_expire_frame(KinectFrame& frame, std::chrono::steady_clock::time_point now) {
    if (frame.bodies.empty()) return false;
    if (frame.number && frame.captured_at != std::chrono::steady_clock::time_point{} &&
        now >= frame.captured_at && now - frame.captured_at <= std::chrono::milliseconds(500)) return false;
    frame.bodies.clear();
    return true;
}

void kinect_prune_identities(const KinectFrame& frame, std::unordered_map<uint32_t, uint32_t>& identities) {
    std::erase_if(identities, [&](const auto& entry) {
        return std::none_of(frame.bodies.begin(), frame.bodies.end(),
                           [&](const KinectBody& body) { return body.tracking_id == entry.first; });
    });
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

void KinectPlayerSlots::retain(KinectFrame& frame) {
    assign(frame);
    // Each player's body once: a sensor that repeats an id (a v2's 64-bit
    // ids are shortened to the 32 the title keeps) must not make a third
    // TRACKED skeleton, which the title's two-entry list cannot hold.
    std::array<bool, players> kept{};
    std::erase_if(frame.bodies, [&](const KinectBody& body) {
        if (!body.tracking_id) return true;
        const auto slot = std::find(tracking_.begin(), tracking_.end(), body.tracking_id);
        if (slot == tracking_.end()) return true;
        bool& taken = kept[size_t(slot - tracking_.begin())];
        if (taken) return true;
        taken = true;
        return false;
    });
}

}
