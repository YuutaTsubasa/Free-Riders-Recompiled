#include "guest_memory.h"
#include "kinect_sensor.h"
#include "pose_skeleton.h"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static float load_float(sfr::GuestMemory& memory, uint64_t address) {
    return std::bit_cast<float>(memory.load<uint32_t>(address));
}
static bool near(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }
static sfr::KinectBody body(uint32_t id, float x) {
    sfr::KinectBody b;
    b.tracking_id = id;
    b.position = {x, 0.0f, 2.0f};
    for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
        b.joints[j] = {x, 0.1f * float(j), 2.0f};
        b.joint_states[j] = j == sfr::nui_joint::foot_left ? 1u : 2u;
    }
    return b;
}

int main() {
    try {
        sfr::KinectPlayerSlots slots;
        sfr::KinectFrame frame;
        auto players = slots.assign(frame);
        require(!players[0] && !players[1], "nobody in front of the sensor fills no slot");

        frame.bodies = {body(77, 0.3f)};
        players = slots.assign(frame);
        require(players[0] && players[0]->tracking_id == 77 && !players[1], "the first body is the first player");

        frame.bodies = {body(90, -0.4f), body(77, 0.3f)};
        players = slots.assign(frame);
        require(players[0]->tracking_id == 77 && players[1] && players[1]->tracking_id == 90,
                "a body that steps in beside the first player takes the second slot, whatever the sensor's order");

        frame.bodies = {body(90, -0.4f)};
        players = slots.assign(frame);
        require(!players[0] && players[1]->tracking_id == 90,
                "the second player keeps their slot when the first one leaves");

        frame.bodies = {body(90, -0.4f), body(12, 0.0f)};
        players = slots.assign(frame);
        require(players[0]->tracking_id == 12 && players[1]->tracking_id == 90,
                "a newcomer takes the free first slot");

        frame.bodies = {body(0, 0.0f)};
        players = slots.assign(frame);
        require(!players[0] && !players[1], "a skeleton without a tracking id is not a player");

        // The frame the title reads: the sensor's floor, its joint states,
        // and the body's own centre.
        constexpr uint32_t address = 0x10000;
        sfr::GuestMemory memory;
        memory.map(address, 0x1000);
        sfr::NuiSkeletonEmulation player;
        sfr::NuiSkeletonEmulation::write_header(memory, address, 3, 100);
        sfr::NuiSkeletonEmulation::write_floor(memory, address, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
        require(load_float(memory, address + 16 + 4) == 1.0f && load_float(memory, address + 16 + 12) == 1.0f,
                "a sensor that cannot see the floor keeps the emulated one");
        sfr::NuiSkeletonEmulation::write_floor(memory, address, {0.02f, 0.99f, -0.1f, 0.8f}, {0.0f, 0.98f, 0.05f});
        require(load_float(memory, address + 16) == 0.02f && load_float(memory, address + 16 + 12) == 0.8f,
                "the sensor's floor plane is the frame's");
        require(load_float(memory, address + 32 + 8) == 0.05f && load_float(memory, address + 32 + 12) == 0.0f,
                "the sensor's gravity is the frame's, as a direction");
        const auto b = body(5, 0.25f);
        player.write_joints(memory, address, 1, 2, b.joints, b.joint_states, b.position);
        const uint32_t data = address + sfr::nui_skeleton_data_offset + sfr::nui_skeleton_data_size;
        require(memory.load<uint32_t>(data) == sfr::nui_tracked && memory.load<uint32_t>(data + 4) == 2,
                "the body is tracked in its slot under the player's tracking id");
        require(load_float(memory, data + 16) == 0.25f && load_float(memory, data + 16 + 8) == 2.0f,
                "the skeleton's position is the sensor's");
        require(load_float(memory, data + 32 + sfr::nui_joint::head * 16 + 4) == b.joints[sfr::nui_joint::head][1],
                "joints are written where the title reads them");
        require(memory.load<uint32_t>(data + 352 + sfr::nui_joint::foot_left * 4) == 1 &&
                    memory.load<uint32_t>(data + 352 + sfr::nui_joint::head * 4) == 2,
                "inferred joints stay inferred");
        require(memory.load<uint32_t>(address + sfr::nui_skeleton_data_offset) == 0, "the empty slot stays empty");

        // Kinect v2: its first twenty joints are v1's, but v1's shoulder
        // centre is v2's spine-shoulder (20), not its neck (2).
        std::array<sfr::KinectV2Joint, sfr::kinect_v2_joint_count> v2{};
        for (uint32_t j = 0; j < sfr::kinect_v2_joint_count; ++j) v2[j] = {{float(j), 0.5f, 2.0f}, 2};
        v2[sfr::nui_joint::hand_left].state = 1;
        v2[sfr::kinect_v2_joint::spine_shoulder].state = 7;
        sfr::KinectBody from_v2;
        sfr::kinect_v2_body(v2, 42, from_v2);
        require(from_v2.tracking_id == 42, "a v2 body keeps its tracking id");
        require(from_v2.joints[sfr::nui_joint::shoulder_center][0] == float(sfr::kinect_v2_joint::spine_shoulder),
                "v1's shoulder centre comes from v2's spine-shoulder");
        require(from_v2.joint_states[sfr::nui_joint::shoulder_center] == sfr::nui_tracked,
                "a joint state past tracked is read as tracked");
        for (uint32_t j = 0; j < sfr::nui_joint_count; ++j)
            if (j != sfr::nui_joint::shoulder_center)
                require(from_v2.joints[j][0] == float(j), "the other joints keep their places");
        require(from_v2.joint_states[sfr::nui_joint::hand_left] == 1, "an inferred v2 joint stays inferred");
        require(from_v2.position[0] == float(sfr::kinect_v2_joint::spine_base), "the body's centre is its spine base");

        // Why neither sensor opened: the reason of a runtime that is there.
        require(sfr::kinect_open_failure("no-runtime", "no-runtime") == "no-runtime", "nothing installed");
        require(sfr::kinect_open_failure("no-sensor", "no-runtime") == "no-sensor", "only SDK 1.8, no sensor");
        require(sfr::kinect_open_failure("no-runtime", "no-sensor") == "no-sensor", "only SDK 2.0, no sensor");
        require(sfr::kinect_open_failure("no-sensor", "initialize-0x80070005") == "initialize-0x80070005",
                "a v2 that is there but will not start says more than a missing v1");
        require(sfr::kinect_open_failure("initialize-0x8007048F", "no-sensor") == "initialize-0x8007048F",
                "a v1 that will not start says more than a missing v2");

        // Placement: a sensor at the player's side or behind is turned into
        // the front sensor's space, measured from where the body was found.
        require(sfr::kinect_placement_from("right") == sfr::KinectPlacement::right &&
                    sfr::kinect_placement_from(nullptr) == sfr::KinectPlacement::front &&
                    sfr::kinect_placement_from("sideways") == sfr::KinectPlacement::front,
                "placements are read by name, front otherwise");
        {
            sfr::KinectFrame seen;
            seen.floor_plane = {0.0f, 1.0f, 0.0f, 1.0f};
            seen.bodies = {body(8, 0.0f)};
            auto& b = seen.bodies[0];
            b.position = {0.2f, 0.0f, 3.0f};
            b.joints[sfr::nui_joint::hip_center] = {0.2f, 0.0f, 3.0f};
            // Leaning 10 cm towards the sensor's left, and 20 cm towards it.
            b.joints[sfr::nui_joint::head] = {0.3f, 0.6f, 2.8f};
            sfr::KinectFrame front = seen;
            sfr::KinectPlacementTransform none;
            none.apply(front);
            require(front.bodies[0].joints[sfr::nui_joint::head] == b.joints[sfr::nui_joint::head] &&
                        front.floor_plane[1] == 1.0f,
                    "a front sensor's frame is left as it came");

            // On the player's right, the sensor's left is away from the
            // screen and its forward is towards the player's left.
            sfr::KinectFrame right = seen;
            sfr::KinectPlacementTransform at_right(sfr::KinectPlacement::right);
            at_right.apply(right);
            const auto& hip = right.bodies[0].joints[sfr::nui_joint::hip_center];
            const auto& head = right.bodies[0].joints[sfr::nui_joint::head];
            require(near(hip[0], 0.0f) && near(hip[2], sfr::pose_distance), "the body stands where the player does");
            require(near(head[0], 0.2f) && near(head[2], sfr::pose_distance + 0.1f) && near(head[1], 0.6f),
                    "towards the right sensor is the player's right; its left is away from the screen");
            require(right.floor_plane[1] == 0.0f, "a turned frame keeps no floor of the sensor's");

            // The anchor stays: moving later moves the body.
            seen.bodies[0].joints[sfr::nui_joint::hip_center] = {0.2f, 0.0f, 2.9f};
            right = seen;
            at_right.apply(right);
            require(near(right.bodies[0].joints[sfr::nui_joint::hip_center][0], 0.1f),
                    "a step towards the side sensor is a step to the player's right");

            sfr::KinectFrame left = seen;
            sfr::KinectPlacementTransform at_left(sfr::KinectPlacement::left);
            left.bodies[0].joints[sfr::nui_joint::hip_center] = {0.2f, 0.0f, 3.0f};
            at_left.apply(left);
            const auto& left_head = left.bodies[0].joints[sfr::nui_joint::head];
            require(near(left_head[0], -0.2f) && near(left_head[2], sfr::pose_distance - 0.1f),
                    "towards the left sensor is the player's left; its left is towards the screen");

            // Behind a side-on rider is a profile, as in front: turned half
            // round, and nothing swapped.
            sfr::KinectFrame behind = seen;
            behind.bodies[0].joints[sfr::nui_joint::hip_center] = {0.2f, 0.0f, 3.0f};
            behind.bodies[0].joints[sfr::nui_joint::hand_left] = {-0.1f, 0.2f, 3.0f};
            sfr::KinectPlacementTransform at_back(sfr::KinectPlacement::behind);
            at_back.apply(behind);
            require(near(behind.bodies[0].joints[sfr::nui_joint::hand_left][0], 0.3f),
                    "behind, the sensor's left is the player's right");
            require(near(behind.bodies[0].joints[sfr::nui_joint::head][2], sfr::pose_distance + 0.2f),
                    "behind, towards the sensor is away from the screen");

            // At the front right (45 degrees), a step towards the sensor's
            // left goes back and right in equal parts.
            sfr::KinectFrame diagonal = seen;
            diagonal.bodies[0].joints[sfr::nui_joint::hip_center] = {0.2f, 0.0f, 3.0f};
            diagonal.bodies[0].joints[sfr::nui_joint::hand_right] = {1.2f, 0.0f, 3.0f};
            sfr::KinectPlacementTransform at_front_right(sfr::KinectPlacement::front_right);
            at_front_right.apply(diagonal);
            const auto& reach = diagonal.bodies[0].joints[sfr::nui_joint::hand_right];
            require(near(reach[0], 0.70710678f) && near(reach[2], sfr::pose_distance + 0.70710678f),
                    "a diagonal sensor turns by 45 degrees");
            require(sfr::kinect_placement_from("behind-left") == sfr::KinectPlacement::behind_left &&
                        sfr::kinect_placement_degrees(sfr::KinectPlacement::behind_left) == 225.0f &&
                        std::string(sfr::kinect_placement_name(sfr::KinectPlacement::front_left)) == "front-left",
                    "the diagonals have names and angles");
        }

        std::string why;
        if (!sfr::KinectSensor::supported())
            require(!sfr::KinectSensor::open(&why) && !why.empty(), "a platform without a runtime says why");
    } catch (const std::exception& error) {
        std::cerr << "kinect_sensor_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
