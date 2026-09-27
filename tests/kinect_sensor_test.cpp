#include "guest_memory.h"
#include "kinect_sensor.h"
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

        std::string why;
        if (!sfr::KinectSensor::supported())
            require(!sfr::KinectSensor::open(&why) && !why.empty(), "a platform without a runtime says why");
    } catch (const std::exception& error) {
        std::cerr << "kinect_sensor_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
