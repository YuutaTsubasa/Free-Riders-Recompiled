#include "guest_memory.h"
#include "kinect_sensor.h"
#include "kinect_preview.h"
#include "pose_skeleton.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <string>
#include <unordered_map>

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

        {
            sfr::KinectPlayerSlots tracked;
            sfr::KinectFrame many;
            const auto six_bodies = [] {
                std::vector<sfr::KinectBody> bodies;
                for (uint32_t index = 0; index < 6; ++index) {
                    auto candidate = body(100 + index, float(index));
                    candidate.sensor_index = index;
                    bodies.push_back(candidate);
                }
                return bodies;
            };
            many.bodies = six_bodies();
            tracked.retain(many);
            require(many.bodies.size() == 2 && many.bodies[0].tracking_id == 100 && many.bodies[1].tracking_id == 101,
                    "at most two fully tracked v2 bodies reach the title's two-entry body list");
            many.bodies = six_bodies();
            std::reverse(many.bodies.begin(), many.bodies.end());
            tracked.retain(many);
            require(many.bodies.size() == 2 && many.bodies[0].tracking_id == 101 && many.bodies[1].tracking_id == 100,
                    "source reordering does not replace either selected player");
            many.bodies = six_bodies();
            many.bodies.erase(many.bodies.begin());
            tracked.retain(many);
            require(many.bodies.size() == 2 && many.bodies[0].tracking_id == 101 && many.bodies[1].tracking_id == 102,
                    "a tracked newcomer replaces only the selected player who leaves");
            require(many.bodies[0].sensor_index == 1 && many.bodies[1].sensor_index == 2,
                    "selected bodies preserve their original sensor slots and depth player indices");
            many.bodies.clear();
            tracked.retain(many);
            many.bodies = {body(200, 0.0f)};
            many.bodies[0].sensor_index = 5;
            tracked.retain(many);
            require(many.bodies.size() == 1 && many.bodies[0].tracking_id == 200 && many.bodies[0].sensor_index == 5,
                    "empty tracking releases selection and a later body keeps its sensor slot");
        }

        {
            // The title keeps the TRACKED skeletons' slots in a two-entry
            // array (82438EB0 writes every TRACKED slot to manager +7740 + 4n):
            // a third overwrites +7748, a fourth the skeleton thread's stop
            // flag (+7752). Whatever a Kinect v2 sees -- three to six bodies,
            // coming, going and changing places, in any order, even ids it
            // repeats -- at most two skeletons may reach the title, each once,
            // in a slot of its own; the players stay put while tracked.
            sfr::KinectPlayerSlots slots_under_test;
            std::mt19937 random(2024);
            std::vector<uint32_t> present;  // tracking ids in front of the sensor
            uint32_t next_id = 1000, frames_checked = 0, most = 0;
            std::array<uint32_t, 2> before{};
            const auto tracked_in_guest_frame = [](const sfr::KinectFrame& frame) {
                // As the skeleton hook writes them (write_kinect_body): each
                // body in its own sensor slot of a cleared frame.
                sfr::GuestMemory guest;
                guest.map(0x30000, 0x1000);
                sfr::NuiSkeletonEmulation::write_header(guest, 0x30000, 1, 0);
                sfr::NuiSkeletonEmulation writer;
                for (const sfr::KinectBody& b : frame.bodies)
                    writer.write_joints(guest, 0x30000, b.sensor_index % 6, b.tracking_id, b.joints, b.joint_states, b.position);
                uint32_t tracked = 0;
                for (uint32_t slot = 0; slot < 6; ++slot)
                    tracked += guest.load<uint32_t>(0x30000 + sfr::nui_skeleton_data_offset + slot * sfr::nui_skeleton_data_size) ==
                               sfr::nui_tracked;
                return tracked;
            };
            for (uint32_t step = 0; step < 3000; ++step) {
                // Someone leaves, someone steps in, keeping three to six around
                // most of the time (and now and then nobody at all).
                if (step % 500 == 499) present.clear();
                if (!present.empty() && random() % 4 == 0) present.erase(present.begin() + random() % present.size());
                while (present.size() < 3 + random() % 4 && present.size() < 6) present.push_back(next_id++);
                std::vector<sfr::KinectBody> bodies;
                std::vector<uint32_t> free_slots = {0, 1, 2, 3, 4, 5};
                std::shuffle(free_slots.begin(), free_slots.end(), random);
                for (size_t i = 0; i < present.size(); ++i) {
                    auto b = body(present[i], float(i));
                    b.sensor_index = free_slots[i];
                    bodies.push_back(b);
                }
                // The sensor's order changes from frame to frame; once in a
                // while a v2 hands the same (shortened) id over twice.
                std::shuffle(bodies.begin(), bodies.end(), random);
                if (step % 97 == 0 && bodies.size() >= 2) {
                    bodies.push_back(bodies.front());
                    bodies.back().sensor_index = (bodies.front().sensor_index + 1) % 6;
                }
                sfr::KinectFrame frame;
                frame.bodies = bodies;
                slots_under_test.retain(frame);
                ++frames_checked;
                most = (std::max)(most, uint32_t(frame.bodies.size()));
                require(frame.bodies.size() <= 2, "never more than two bodies reach the title");
                require(tracked_in_guest_frame(frame) <= 2, "never more than two TRACKED skeletons in the title's frame");
                if (frame.bodies.size() == 2) {
                    require(frame.bodies[0].tracking_id != frame.bodies[1].tracking_id, "each player once");
                    require(frame.bodies[0].sensor_index != frame.bodies[1].sensor_index, "each in a slot of its own");
                }
                for (const sfr::KinectBody& kept : frame.bodies)
                    require(std::find(present.begin(), present.end(), kept.tracking_id) != present.end(),
                            "only someone who is there");
                // A player who is still there is still a player.
                for (uint32_t id : before)
                    if (id && std::find(present.begin(), present.end(), id) != present.end())
                        require(std::any_of(frame.bodies.begin(), frame.bodies.end(),
                                            [&](const sfr::KinectBody& kept) { return kept.tracking_id == id; }),
                                "a tracked player keeps playing while newcomers come and go");
                before = {};
                for (size_t i = 0; i < frame.bodies.size() && i < 2; ++i) before[i] = frame.bodies[i].tracking_id;
            }
            require(frames_checked == 3000 && most == 2, "two players were seen, never more");
        }

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
        sfr::kinect_v2_body(v2, 42, 2, from_v2);
        require(from_v2.tracking_id == 42, "a v2 body keeps its tracking id");
        sfr::KinectBody second_v2;
        sfr::kinect_v2_body(v2, 43, 5, second_v2);
        require(from_v2.sensor_index == 2 && second_v2.sensor_index == 5,
                "two v2 bodies retain their distinct source slots instead of overwriting slot zero");
        require(from_v2.joints[sfr::nui_joint::shoulder_center][0] == float(sfr::kinect_v2_joint::spine_shoulder),
                "v1's shoulder centre comes from v2's spine-shoulder");
        require(from_v2.joint_states[sfr::nui_joint::shoulder_center] == sfr::nui_tracked,
                "a joint state past tracked is read as tracked");
        for (uint32_t j = 0; j < sfr::nui_joint_count; ++j)
            if (j != sfr::nui_joint::shoulder_center)
                require(from_v2.joints[j][0] == float(j), "the other joints keep their places");
        require(from_v2.joint_states[sfr::nui_joint::hand_left] == 1, "an inferred v2 joint stays inferred");
        require(from_v2.position[0] == float(sfr::kinect_v2_joint::spine_base), "the body's centre is its spine base");

        {
            // A skeleton-only backend must not claim either image stream.
            struct SkeletonOnly final : sfr::KinectSensor {
                bool next(sfr::KinectFrame&) override { return false; }
                const char* model() const override { return "skeleton only"; }
            } sensor;
            require(!sensor.has_depth_stream() && !sensor.has_colour_stream(),
                    "image capabilities default to absent for skeleton-only backends");
        }
        {
            using Clock = std::chrono::steady_clock;
            using namespace std::chrono_literals;
            const auto acquired = Clock::time_point{} + 10s;
            sfr::KinectFrame sample;
            require(!sfr::kinect_expire_frame(sample, acquired) && sample.bodies.empty(),
                    "startup without a frame stays empty");
            sample.number = 1;
            sample.captured_at = acquired;
            sample.bodies = {body(77, 0.3f)};
            std::unordered_map<uint32_t, uint32_t> identities{{77, 0}};
            require(!sfr::kinect_expire_frame(sample, acquired + 500ms) && sample.bodies.size() == 1,
                    "a short sensor stall keeps its last real body within the grace period");
            require(sfr::kinect_expire_frame(sample, acquired + 501ms) && sample.bodies.empty(),
                    "a disconnected sensor expires its tracked body after 500 ms");
            sfr::kinect_prune_identities(sample, identities);
            require(identities.empty(), "expiring tracking also discards the old enrollment");
            require(sample.captured_at == acquired && sample.number == 1,
                    "expiry never stamps the cached sample as newly captured");
            require(!sfr::kinect_expire_frame(sample, acquired + 900ms),
                    "an expired frame is not repeatedly reported as a tracking change");

            // The consumer may first see a sample long after its worker acquired it.
            sample.number = 2;
            sample.bodies = {body(88, -0.2f)};
            sample.captured_at = acquired + 1s;
            require(sfr::kinect_expire_frame(sample, acquired + 1600ms) && sample.bodies.empty(),
                    "a delayed consumer uses acquisition time rather than receipt time");
            sample.number = 3;
            sample.captured_at = acquired + 2s;
            sample.bodies = {body(99, 0.1f)};
            require(!sfr::kinect_expire_frame(sample, acquired + 2010ms) && sample.bodies[0].tracking_id == 99,
                    "a new captured frame recovers tracking after disconnect expiry");
            sample.number = 4;
            sample.captured_at = acquired + 2100ms;
            sample.bodies.clear();
            identities = {{99, 0}};
            require(!sfr::kinect_expire_frame(sample, acquired + 2110ms) && sample.bodies.empty(),
                    "a fresh empty frame clears tracking immediately without a grace period");
            sfr::kinect_prune_identities(sample, identities);
            require(identities.empty(), "fresh empty frames clear identities too");
        }

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
            using sfr::KinectStep;
            using sfr::KinectStepState;
            const auto state = [](const sfr::KinectReadiness& r, KinectStep step) { return r.steps[size_t(step)]; };
            sfr::KinectReadinessInput input;
            auto r = sfr::kinect_readiness(input);
            require(state(r, KinectStep::runtime) == KinectStepState::waiting && r.first_failed == KinectStep::count,
                    "while the sensor opens every step waits");

            input.failure = "no-runtime";
            r = sfr::kinect_readiness(input);
            require(r.first_failed == KinectStep::runtime && state(r, KinectStep::sensor) == KinectStepState::waiting,
                    "without a runtime the runtime step fails and the sensor is not asked");
            input.failure = "no-sensor";
            r = sfr::kinect_readiness(input);
            require(state(r, KinectStep::runtime) == KinectStepState::passed && r.first_failed == KinectStep::sensor,
                    "a runtime without a sensor fails at the sensor");
            input.failure = "initialize-0x80080014";
            require(sfr::kinect_readiness(input).first_failed == KinectStep::sensor, "a sensor that will not start fails at the sensor");

            input = {};
            input.opened = true;
            input.seconds_open = 1.0;
            r = sfr::kinect_readiness(input);
            require(state(r, KinectStep::sensor) == KinectStepState::passed &&
                    state(r, KinectStep::frames) == KinectStepState::waiting && r.first_failed == KinectStep::count,
                    "a sensor just opened waits for its first frame");
            input.seconds_open = 4.0;
            require(sfr::kinect_readiness(input).first_failed == KinectStep::frames,
                    "an open sensor that sends nothing fails at the frames");
            input.seconds_since_frame = 3.0;
            input.seconds_open = 10.0;
            require(sfr::kinect_readiness(input).first_failed == KinectStep::frames, "frames that stopped fail at the frames");
            input.seconds_since_frame = 0.05;
            r = sfr::kinect_readiness(input);
            require(r.first_failed == KinectStep::body && !r.ready(), "frames with nobody in them fail at the body");
            input.bodies = 1;
            r = sfr::kinect_readiness(input);
            require(r.ready() && r.first_failed == KinectStep::count, "a tracked body passes every step");
        }

        {
            // The adjustment window's body: arms hanging below the shoulders,
            // under the shoulder line, so no hand is raised as a menu cursor.
            namespace joint = sfr::nui_joint;
            sfr::KinectBody body;
            body.joints[joint::hip_center] = {0.0f, 0.0f, 2.4f};
            body.joints[joint::shoulder_center] = {0.0f, 0.5f, 2.4f};
            body.joints[joint::shoulder_left] = {-0.18f, 0.5f, 2.4f};
            body.joints[joint::shoulder_right] = {0.18f, 0.5f, 2.4f};
            body.joints[joint::hand_right] = {0.3f, 0.9f, 2.0f};  // raised, reaching
            body.joints[joint::hand_left] = {-0.2f, 0.7f, 2.1f};
            body.joint_states[joint::hand_right] = 1;
            sfr::kinect_arms_down(body);
            for (uint32_t hand : {joint::hand_left, joint::wrist_left, joint::elbow_left,
                                  joint::hand_right, joint::wrist_right, joint::elbow_right}) {
                require(body.joints[hand][1] < 0.5f - 0.2f, "every arm point hangs below the shoulders");
                require(body.joints[hand][2] == 2.4f && body.joint_states[hand] == 2, "at the body's depth, tracked");
            }
            require(body.joints[joint::hand_right][0] == 0.18f && body.joints[joint::hand_left][0] == -0.18f,
                    "each hand below its own shoulder");
            require(body.joints[joint::hand_right][1] < body.joints[joint::elbow_right][1], "the hand below the elbow");
        }

        // The launcher's Kinect choice (SFR_KINECT_MODEL): 2 is the Kinect
        // v2; unset, 1 or anything else is the Xbox 360 Kinect.
        {
#ifdef _WIN32
            const auto set_model = [](const char* value) { _putenv_s("SFR_KINECT_MODEL", value); };
#else
            const auto set_model = [](const char* value) {
                if (*value) setenv("SFR_KINECT_MODEL", value, 1);
                else unsetenv("SFR_KINECT_MODEL");
            };
#endif
            set_model("");
            require(sfr::kinect_selected_model() == 1, "no model chosen is the Xbox 360 Kinect");
            set_model("1");
            require(sfr::kinect_selected_model() == 1, "model 1 is the Xbox 360 Kinect");
            set_model("2");
            require(sfr::kinect_selected_model() == 2, "model 2 is the Kinect v2");
            set_model("3");
            require(sfr::kinect_selected_model() == 1, "an unknown model is the Xbox 360 Kinect");
            set_model("");
        }

        {
            // Which edges cut a body: names, and what the player should do.
            using namespace sfr;
            require(kinect_edges_text(0) == "none", "no edge is none");
            require(kinect_edges_text(kinect_edge::top) == "top", "top");
            require(kinect_edges_text(kinect_edge::left | kinect_edge::top) == "top+left", "edges in a fixed order");
            require(kinect_edges_text(kinect_edge::all) == "top+bottom+left+right", "all four");
            require(kinect_edge::right == 0x1 && kinect_edge::left == 0x2 && kinect_edge::top == 0x4 && kinect_edge::bottom == 0x8,
                    "the SDK's FrameEdges values");
            require(kinect_edges_move(0) == 0, "nothing cut, nothing to do");
            require(kinect_edges_move(kinect_edge::top) == (kinect_edge_move::step_back | kinect_edge_move::tilt_up), "top: back or tilt up");
            require(kinect_edges_move(kinect_edge::bottom) == (kinect_edge_move::step_back | kinect_edge_move::tilt_down),
                    "bottom: back or tilt down");
            // The picture's left is the player's right (facing the sensor): step to the player's own left.
            require(kinect_edges_move(kinect_edge::left) == kinect_edge_move::player_left, "picture left: the player's left");
            require(kinect_edges_move(kinect_edge::right) == kinect_edge_move::player_right, "picture right: the player's right");
            require(kinect_edges_move(kinect_edge::left | kinect_edge::right) == kinect_edge_move::step_back, "both sides: step back");
            require(kinect_edges_move(kinect_edge::top | kinect_edge::bottom) == kinect_edge_move::step_back, "both ends: step back");
            require(kinect_edges_move(kinect_edge::top | kinect_edge::left) ==
                        (kinect_edge_move::step_back | kinect_edge_move::tilt_up | kinect_edge_move::player_left), "a corner needs both");
            KinectBody body;
            require(body.clipped_edges == 0, "a body (a v1's) is not cut by default");
        }

        std::string why;
        if (!sfr::KinectSensor::supported()) {
            require(!sfr::KinectSensor::open(&why) && !why.empty(), "a platform without a runtime says why");
            why.clear();
            require(!sfr::KinectSensor::open_selected(1, &why) && !why.empty(), "the Xbox 360 choice says why it found nothing");
            why.clear();
            require(!sfr::KinectSensor::open_selected(2, &why) && !why.empty(), "the Kinect v2 choice says why it found nothing");
        }
    } catch (const std::exception& error) {
        std::cerr << "kinect_sensor_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
