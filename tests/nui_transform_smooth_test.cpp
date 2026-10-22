#include "guest_memory.h"
#include "nui_transform_smooth.h"

#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>

// NuiTransformSmooth as the title's NUI library has it (82779700), worked
// by hand (nui_transform_smooth.h describes the steps).
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(float a, float b, float tolerance = 1e-6f) { return std::fabs(a - b) <= tolerance; }

float step(sfr::NuiSmoothJoint& joint, float x, uint32_t state = 2, const sfr::NuiSmoothParameters& p = {}) {
    const auto out = sfr::nui_smooth_joint(joint, {x, 1.0f, 2.0f}, state, p);
    require(near(out[1], 1.0f) && near(out[2], 2.0f), "an axis that does not move stays put");
    return out[0];
}

void the_first_frames() {
    sfr::NuiSmoothJoint joint;
    require(step(joint, 0.0f) == 0.0f, "the first frame is the measurement");
    // Second: the mean (0.05), trend 0.5 * 0.05; output 0.05 + 0.5 * 0.025.
    require(near(step(joint, 0.1f), 0.0625f), "the second frame is the mean plus half the trend");
    // Third: moved 0.05 from the last filtered value, at the jitter radius:
    // taken whole; filtered 0.5 * 0.1 + 0.5 * (0.05 + 0.025) = 0.0875, trend
    // 0.5 * 0.0375 + 0.5 * 0.025 = 0.03125, output 0.0875 + 0.015625.
    require(near(step(joint, 0.1f), 0.103125f), "then Holt's double exponential step");
    sfr::NuiSmoothJoint still;
    for (int i = 0; i < 30; ++i) require(near(step(still, 0.2f), 0.2f), "a joint that does not move is left where it is");
}

void the_prediction_is_held_back() {
    // Two frames at 0, then a 1 m jump: filtered 0.5, trend 0.25, prediction
    // 0.625 is 0.375 from the measurement, drawn back to 0.04 (0.96).
    sfr::NuiSmoothJoint tracked, inferred;
    for (int i = 0; i < 2; ++i) { step(tracked, 0.0f); step(inferred, 0.0f, 1); }
    require(near(step(tracked, 1.0f), 0.96f, 1e-5f), "never more than the deviation radius from the measurement");
    require(near(step(inferred, 1.0f, 1), 0.92f, 1e-5f), "an inferred joint has twice the radius");
}

void jitter_is_pulled_in() {
    // After two frames at 0, a move of 0.02 (inside 0.05): pulled to
    // 0.02 * 0.4 = 0.008, filtered 0.004, trend 0.002, output 0.005.
    sfr::NuiSmoothJoint joint;
    step(joint, 0.0f);
    step(joint, 0.0f);
    require(near(step(joint, 0.02f), 0.005f), "a small move is damped in proportion to its size");
    // The radius is never under 0.0001 m: a 0.00005 move with a radius of 0
    // is still pulled halfway (0.000025), filtered 0.0000125.
    sfr::NuiSmoothParameters none;
    none.jitter_radius = 0.0f;
    sfr::NuiSmoothJoint small;
    step(small, 0.0f, 2, none);
    step(small, 0.0f, 2, none);
    require(near(step(small, 0.00005f, 2, none), 0.0000125f + 0.5f * 0.00000625f, 1e-9f), "the smallest jitter radius");
}

void lost_joints_start_over() {
    sfr::NuiSmoothJoint joint;
    for (int i = 0; i < 10; ++i) step(joint, 0.3f);
    const auto lost = sfr::nui_smooth_joint(joint, {0.0f, 0.0f, 0.0f}, 0, {});
    require(lost[0] == 0.0f && lost[1] == 0.0f && lost[2] == 0.0f, "a joint at the origin passes as it is");
    // It was the first frame of a new history, so the next one is the mean
    // with the origin (the library's way, and the SDK sample's), held back
    // to the deviation radius from the measurement.
    const std::array<float, 3> back{0.9f, 1.0f, 2.0f};
    const auto after = sfr::nui_smooth_joint(joint, back, 2, {});
    const float off = std::sqrt((after[0] - back[0]) * (after[0] - back[0]) + (after[1] - back[1]) * (after[1] - back[1]) +
                                (after[2] - back[2]) * (after[2] - back[2]));
    require(near(off, 0.04f, 1e-5f) && after[0] < back[0], "the frame after a lost one starts over from the origin");
    // A joint the sensor calls not tracked but gives a position is filtered
    // like a tracked one (the library looks only at the position).
    sfr::NuiSmoothJoint untracked;
    step(untracked, 0.0f, 0);
    require(near(step(untracked, 0.1f, 0), 0.0625f), "not tracked with a position: filtered");
}

sfr::NuiFrameSmoother::Slot slot_at(uint32_t id, float x) {
    sfr::NuiFrameSmoother::Slot slot;
    slot.tracking_state = 2;
    slot.tracking_id = id;
    for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
        slot.joints[j] = {x, 0.1f * float(j), 2.0f};
        slot.states[j] = 2;
    }
    return slot;
}

void slots_keep_their_own_history() {
    sfr::NuiFrameSmoother smoother;
    std::array<sfr::NuiFrameSmoother::Slot, 6> slots{};
    slots[1] = slot_at(5, 0.0f);
    smoother.smooth(slots, {});
    slots[1] = slot_at(5, 0.1f);
    smoother.smooth(slots, {});
    require(near(slots[1].joints[4][0], 0.0625f), "a tracked slot is filtered");
    // Another person in the same slot starts from their own measurement.
    slots[1] = slot_at(6, 0.7f);
    smoother.smooth(slots, {});
    require(slots[1].joints[4][0] == 0.7f, "a new tracking id starts the slot over");
    // A slot that is not tracked is left alone and forgets.
    slots[1].tracking_state = 1;
    slots[1].joints[4][0] = 0.4f;
    smoother.smooth(slots, {});
    require(slots[1].joints[4][0] == 0.4f, "a position-only skeleton is not filtered");
    slots[1] = slot_at(6, 0.2f);
    smoother.smooth(slots, {});
    require(slots[1].joints[4][0] == 0.2f, "and its slot starts over when it is tracked again");
}

// The title's own big-endian frame.
constexpr uint32_t frame = 0x20000, parameters = 0x21000;
void store_float(sfr::GuestMemory& memory, uint64_t address, float value) {
    memory.store<uint32_t>(address, std::bit_cast<uint32_t>(value));
}
float load_float(sfr::GuestMemory& memory, uint64_t address) { return std::bit_cast<float>(memory.load<uint32_t>(address)); }
void write_frame(sfr::GuestMemory& memory, uint32_t number, float x) {
    for (uint32_t offset = 0; offset < sfr::nui_skeleton_frame_size; offset += 4) memory.store<uint32_t>(frame + offset, 0);
    memory.store<uint32_t>(frame + 8, number);
    const uint64_t data = frame + sfr::nui_skeleton_data_offset + 3 * sfr::nui_skeleton_data_size;
    memory.store<uint32_t>(data, 2);
    memory.store<uint32_t>(data + 4, 9);
    store_float(memory, data + 16, x);  // the centre
    for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
        store_float(memory, data + 32 + 16 * j, x);
        store_float(memory, data + 32 + 16 * j + 4, 1.0f);
        store_float(memory, data + 32 + 16 * j + 8, 2.0f);
        store_float(memory, data + 32 + 16 * j + 12, 0.5f);
        memory.store<uint32_t>(data + 352 + 4 * j, 2);
    }
    // An untracked slot with stale numbers in it.
    const uint64_t other = frame + sfr::nui_skeleton_data_offset;
    store_float(memory, other + 32, 3.0f);
}

void the_guest_frame() {
    sfr::GuestMemory memory;
    memory.map(frame, 0x2000);
    sfr::NuiFrameSmoother smoother;
    require(sfr::nui_transform_smooth_guest(memory, 0, 0, smoother) == 0x80004003u, "no frame is E_POINTER");
    const uint64_t joint = frame + sfr::nui_skeleton_data_offset + 3 * sfr::nui_skeleton_data_size + 32 + 16 * 11;
    write_frame(memory, 1, 0.0f);
    require(sfr::nui_transform_smooth_guest(memory, frame, 0, smoother) == 0, "S_OK");
    write_frame(memory, 2, 0.1f);
    sfr::nui_transform_smooth_guest(memory, frame, 0, smoother);
    require(near(load_float(memory, joint), 0.0625f), "no parameters are the library's defaults");
    require(load_float(memory, joint + 12) == 1.0f, "each joint's w becomes 1");
    require(load_float(memory, frame + sfr::nui_skeleton_data_offset + 3 * sfr::nui_skeleton_data_size + 16) == 0.1f,
            "the skeleton's centre is not filtered");
    require(load_float(memory, frame + sfr::nui_skeleton_data_offset + 32) == 3.0f, "an untracked slot is left alone");
    // The skeleton hook handing the same sensor frame over again: the same
    // output, not a second step towards the measurement.
    write_frame(memory, 2, 0.1f);
    sfr::nui_transform_smooth_guest(memory, frame, 0, smoother);
    require(near(load_float(memory, joint), 0.0625f), "a repeated frame is not smoothed twice");
    write_frame(memory, 3, 0.1f);
    sfr::nui_transform_smooth_guest(memory, frame, 0, smoother);
    require(near(load_float(memory, joint), 0.103125f), "the next frame goes on from the first time");
    // Given parameters, big-endian: no smoothing, no correction, no
    // prediction is the measurement itself.
    sfr::NuiFrameSmoother plain;
    for (uint32_t i = 0; i < 5; ++i) store_float(memory, parameters + 4 * i, i < 3 ? 0.0f : 1.0f);
    const auto given = sfr::nui_smooth_parameters(memory, parameters);
    require(given.smoothing == 0.0f && given.jitter_radius == 1.0f && given.max_deviation_radius == 1.0f, "read big-endian");
    store_float(memory, parameters + 12, 0.0f);
    for (uint32_t number = 10; number < 14; ++number) {
        write_frame(memory, number, 0.1f * float(number));
        sfr::nui_transform_smooth_guest(memory, frame, parameters, plain);
    }
    require(near(load_float(memory, joint), 1.3f, 1e-5f), "smoothing 0 follows the measurement");
}
}

int main() {
    try {
        the_first_frames();
        the_prediction_is_held_back();
        jitter_is_pulled_in();
        lost_joints_start_over();
        slots_keep_their_own_history();
        the_guest_frame();
        std::cout << "NuiTransformSmooth checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
