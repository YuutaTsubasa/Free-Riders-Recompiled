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

void KinectPlacementTransform::apply(KinectFrame& frame) {
    if (placement_ == KinectPlacement::front) return;
    frame.floor_plane = {};
    frame.gravity = {};
    // Bodies no longer tracked are forgotten, so whoever steps in next is
    // measured from where they stand.
    std::erase_if(tracked_, [&](const Tracked& entry) {
        return std::none_of(frame.bodies.begin(), frame.bodies.end(),
                            [&](const KinectBody& body) { return body.tracking_id == entry.id; });
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
        auto entry = std::find_if(tracked_.begin(), tracked_.end(),
                                  [&](const Tracked& known) { return known.id == body.tracking_id; });
        if (entry == tracked_.end()) entry = tracked_.insert(tracked_.end(), Tracked{body.tracking_id, body.position});
        const std::array<float, 3> origin = entry->anchor;
        const auto turn = [&](const std::array<float, 3>& p) -> std::array<float, 3> {
            const float x = p[0] - origin[0], z = p[2] - origin[2];
            return {x * c - z * s, p[1], pose_distance + x * s + z * c};
        };
        for (auto& joint : body.joints) joint = turn(joint);
        body.position = turn(body.position);

        // The chest faces where the toes point: across the screen for a
        // side-on rider, +x (the player's right) or -x. The sensor at angle a
        // is towards +x when sin a > 0. In front and behind (sin a = 0) it
        // sees a profile in either stance, and nothing is changed.
        using namespace nui_joint;
        if (std::fabs(s) > 0.5f) {
            // A kick boost lifts a foot and puts it back, and a lifted foot
            // dangles: only a foot on the ground says where the toes point.
            const float left_toes = body.joints[foot_left][0] - body.joints[ankle_left][0];
            const float right_toes = body.joints[foot_right][0] - body.joints[ankle_right][0];
            const float lift = body.joints[ankle_left][1] - body.joints[ankle_right][1];  // > 0: the left is up
            const float toes = lift > lifted_foot ? right_toes : lift < -lifted_foot ? left_toes
                                                                                     : 0.5f * (left_toes + right_toes);
            const float towards_sensor = toes * (s > 0 ? 1.0f : -1.0f);
            // The shoulders' line, measured and not named, so a swap does not
            // matter: across the screen (x) when the rider faces it or turns
            // away, along it (z) when side-on.
            const float across = body.joints[shoulder_right][0] - body.joints[shoulder_left][0];
            const float along = body.joints[shoulder_right][2] - body.joints[shoulder_left][2];
            const float span = std::sqrt(across * across + along * along);
            if (span > 0.1f && std::fabs(across) >= open_shoulders * span) {
                // A turn is passing: what the feet said before it no longer counts.
                entry->since_open = 0;
                entry->streak = 0;
            } else {
                ++entry->since_open;
            }
            if (std::fabs(towards_sensor) >= least_toe) {
                const bool back = towards_sensor < 0;
                entry->streak = back == entry->back ? 0 : entry->streak + 1;
                const bool may_turn = !entry->told || entry->since_open <= turn_window;
                if (entry->streak >= frames_to_turn && may_turn) {
                    entry->back = back;
                    entry->streak = 0;
                }
                if (entry->streak == 0) entry->told = true;
            }
        }
        if (entry->back) {
            // Its left hand is the player's right: swap each pair back.
            constexpr std::pair<uint32_t, uint32_t> pairs[] = {
                {shoulder_left, shoulder_right}, {elbow_left, elbow_right}, {wrist_left, wrist_right},
                {hand_left, hand_right},         {hip_left, hip_right},     {knee_left, knee_right},
                {ankle_left, ankle_right},       {foot_left, foot_right}};
            for (const auto& [left, right] : pairs) {
                std::swap(body.joints[left], body.joints[right]);
                std::swap(body.joint_states[left], body.joint_states[right]);
            }
        }
    }
}

bool KinectPlacementTransform::sees_back(uint32_t tracking_id) const {
    for (const Tracked& entry : tracked_)
        if (entry.id == tracking_id) return entry.back;
    return false;
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
