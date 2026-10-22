#include "nui_transform_smooth.h"
#include "guest_memory.h"

#include <bit>

// NuiTransformSmooth on the title's own frame (nui_transform_smooth.h); apart
// from the filter so that a host program without guest memory can link it.
namespace sfr {
namespace {
constexpr uint32_t e_pointer = 0x80004003u;
float load_float(GuestMemory& memory, uint64_t address) { return std::bit_cast<float>(memory.load<uint32_t>(address)); }
void store_float(GuestMemory& memory, uint64_t address, float value) {
    memory.store<uint32_t>(address, std::bit_cast<uint32_t>(value));
}
}

NuiSmoothParameters nui_smooth_parameters(GuestMemory& memory, uint32_t address) {
    NuiSmoothParameters parameters;  // the library's own defaults for none
    if (!address) return parameters;
    parameters.smoothing = load_float(memory, address);
    parameters.correction = load_float(memory, uint64_t(address) + 4);
    parameters.prediction = load_float(memory, uint64_t(address) + 8);
    parameters.jitter_radius = load_float(memory, uint64_t(address) + 12);
    parameters.max_deviation_radius = load_float(memory, uint64_t(address) + 16);
    return parameters;
}

uint32_t nui_transform_smooth_guest(GuestMemory& memory, uint32_t frame, uint32_t parameters, NuiFrameSmoother& smoother) {
    if (!frame) return e_pointer;
    memory.check_write(frame, nui_skeleton_frame_size);
    const NuiSmoothParameters given = nui_smooth_parameters(memory, parameters);
    const uint32_t frame_number = memory.load<uint32_t>(uint64_t(frame) + 8);
    std::array<NuiFrameSmoother::Slot, 6> slots;
    for (uint32_t s = 0; s < 6; ++s) {
        const uint64_t data = uint64_t(frame) + nui_skeleton_data_offset + uint64_t(s) * nui_skeleton_data_size;
        NuiFrameSmoother::Slot& slot = slots[s];
        slot.tracking_state = memory.load<uint32_t>(data);
        slot.tracking_id = memory.load<uint32_t>(data + 4);
        for (uint32_t j = 0; j < nui_joint_count; ++j) {
            for (uint32_t axis = 0; axis < 3; ++axis) slot.joints[j][axis] = load_float(memory, data + 32 + 16 * j + 4 * axis);
            slot.states[j] = memory.load<uint32_t>(data + 352 + 4 * j);
        }
    }
    if (smoother.has_last && frame_number == smoother.last_frame) {
        // The same sensor frame again: its output again, not a second step.
        for (uint32_t s = 0; s < 6; ++s)
            if (slots[s].tracking_state == nui_tracked && smoother.last[s].tracking_state == nui_tracked &&
                slots[s].tracking_id == smoother.last[s].tracking_id)
                slots[s].joints = smoother.last[s].joints;
    } else {
        smoother.smooth(slots, given);
        smoother.has_last = true;
        smoother.last_frame = frame_number;
        smoother.last = slots;
    }
    for (uint32_t s = 0; s < 6; ++s) {
        if (slots[s].tracking_state != nui_tracked) continue;
        const uint64_t data = uint64_t(frame) + nui_skeleton_data_offset + uint64_t(s) * nui_skeleton_data_size;
        for (uint32_t j = 0; j < nui_joint_count; ++j) {
            for (uint32_t axis = 0; axis < 3; ++axis)
                store_float(memory, data + 32 + 16 * j + 4 * axis, slots[s].joints[j][axis]);
            store_float(memory, data + 32 + 16 * j + 12, 1.0f);
        }
    }
    return 0;
}

}
