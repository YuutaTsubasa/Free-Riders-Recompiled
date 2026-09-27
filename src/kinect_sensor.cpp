#include "kinect_sensor.h"
#include "pose_skeleton.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sfr {

KinectSensor::~KinectSensor() = default;

namespace {
constexpr const char* placement_names[] = {"front",  "front-right", "right", "behind-right",
                                           "behind", "behind-left", "left",  "front-left"};
}

KinectPlacement kinect_placement_from(const char* text) {
    const std::string value = text ? text : "";
    for (uint8_t i = 0; i < std::size(placement_names); ++i)
        if (value == placement_names[i]) return KinectPlacement(i);
    return KinectPlacement::front;
}

const char* kinect_placement_name(KinectPlacement placement) { return placement_names[uint8_t(placement) % 8]; }

float kinect_placement_degrees(KinectPlacement placement) { return 45.0f * float(uint8_t(placement) % 8); }

void KinectPlacementTransform::apply(KinectFrame& frame) {
    if (placement_ == KinectPlacement::front) return;
    frame.floor_plane = {};
    frame.gravity = {};
    // Anchors of bodies no longer tracked are forgotten, so whoever steps in
    // next is measured from where they stand.
    std::erase_if(anchors_, [&](const auto& anchor) {
        return std::none_of(frame.bodies.begin(), frame.bodies.end(),
                            [&](const KinectBody& body) { return body.tracking_id == anchor.first; });
    });
    // Every sensor's camera space has +x on its own left, +y up and +z
    // towards the player; the front sensor's +x is the player's right. A
    // sensor at angle a (clockwise from the screen) looks along
    // (-sin a, 0, cos a) of the front's space, and its left is
    // (cos a, 0, sin a): its x and z come out as the front's
    //   x = x cos a - z sin a,   z = x sin a + z cos a.
    // Exact quarter turns stay exact.
    const float degrees = kinect_placement_degrees(placement_);
    const float radians = degrees * 3.14159265358979f / 180.0f;
    const float c = degrees == 90.0f || degrees == 270.0f ? 0.0f : degrees == 180.0f ? -1.0f : std::cos(radians);
    const float s = degrees == 180.0f ? 0.0f : degrees == 90.0f ? 1.0f : degrees == 270.0f ? -1.0f : std::sin(radians);
    for (KinectBody& body : frame.bodies) {
        auto anchor = std::find_if(anchors_.begin(), anchors_.end(),
                                   [&](const auto& entry) { return entry.first == body.tracking_id; });
        if (anchor == anchors_.end()) anchor = anchors_.insert(anchors_.end(), {body.tracking_id, body.position});
        const std::array<float, 3> origin = anchor->second;
        const auto turn = [&](const std::array<float, 3>& p) -> std::array<float, 3> {
            const float x = p[0] - origin[0], z = p[2] - origin[2];
            return {x * c - z * s, p[1], pose_distance + x * s + z * c};
        };
        for (auto& joint : body.joints) joint = turn(joint);
        body.position = turn(body.position);
    }
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
