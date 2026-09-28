#include "guest_memory.h"
#include "kinect_sensor.h"
#include "kinect_preview.h"
#include "pose_skeleton.h"
#include <bit>
#include <cmath>
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

        // The preview's projections: a point straight ahead is the image's
        // centre, and up is towards the top.
        {
            const auto centre = sfr::kinect_depth_point({0.0f, 0.0f, 2.0f});
            require(centre.visible && near(centre.x, 160.0f) && near(centre.y, 120.0f), "straight ahead is the depth centre");
            const auto up = sfr::kinect_colour_point({0.0f, 0.5f, 2.0f});
            require(up.visible && near(up.x, 320.0f) && up.y < 240.0f, "up is towards the colour image's top");
            require(!sfr::kinect_depth_point({0.0f, 0.0f, 0.0f}).visible, "a point at the sensor is nowhere");
        }

        // Why neither sensor opened: the reason of a runtime that is there.
        require(sfr::kinect_open_failure("no-runtime", "no-runtime") == "no-runtime", "nothing installed");
        require(sfr::kinect_open_failure("no-sensor", "no-runtime") == "no-sensor", "only SDK 1.8, no sensor");
        require(sfr::kinect_open_failure("no-runtime", "no-sensor") == "no-sensor", "only SDK 2.0, no sensor");
        require(sfr::kinect_open_failure("no-sensor", "initialize-0x80070005") == "initialize-0x80070005",
                "a v2 that is there but will not start says more than a missing v1");
        require(sfr::kinect_open_failure("initialize-0x8007048F", "no-sensor") == "initialize-0x8007048F",
                "a v1 that will not start says more than a missing v2");

        {
            // A sensor pitched up by 15 degrees sees an upright player leaning
            // back: its up is (0, cos, sin) in its own space. Levelled, the
            // body is upright again and gravity is straight down.
            const float t = 15.0f * 3.14159265f / 180.0f, c = std::cos(t), s = std::sin(t);
            const auto seen = [&](float y, float z) { return std::array<float, 3>{0.0f, y * c - z * s, y * s + z * c}; };
            sfr::KinectFrame tilted;
            tilted.gravity = {0.0f, c, s};
            tilted.floor_plane = {0.0f, c, s, 0.54f};
            sfr::KinectBody rider;
            rider.tracking_id = 5;
            for (auto& joint : rider.joints) joint = seen(0.0f, 2.2f);
            rider.joints[sfr::nui_joint::hip_center] = seen(0.0f, 2.2f);
            rider.joints[sfr::nui_joint::head] = seen(0.7f, 2.2f);
            rider.position = seen(0.0f, 2.2f);
            tilted.bodies = {rider};
            require(near(sfr::kinect_level(tilted), 15.0f), "the tilt taken out is the sensor's pitch");
            const auto& hip = tilted.bodies[0].joints[sfr::nui_joint::hip_center];
            const auto& head = tilted.bodies[0].joints[sfr::nui_joint::head];
            require(near(head[2], hip[2]) && near(head[1] - hip[1], 0.7f), "a levelled body stands upright");
            require(near(tilted.gravity[1], 1.0f) && near(tilted.floor_plane[1], 1.0f) &&
                    near(tilted.floor_plane[2], 0.0f) && near(tilted.floor_plane[3], 0.54f),
                    "gravity and the floor are straight up after levelling");

            sfr::KinectFrame upside;
            upside.gravity = {0.0f, -1.0f, 0.0f};
            upside.bodies = {rider};
            require(sfr::kinect_level(upside) == 0.0f && upside.bodies[0].joints[3] == rider.joints[3],
                    "gravity the other way up is not a tilt to take out");
            sfr::KinectFrame none;
            none.bodies = {rider};
            require(sfr::kinect_level(none) == 0.0f, "a frame without gravity is left alone");
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
