#pragma once
#include "plume_render_interface_types.h"
#include <cstdint>
#include <optional>

namespace sfr {
enum class RenderState : uint32_t {
    depth_enable = 0x28, depth_function = 0x2C, depth_write = 0x30,
    cull_mode = 0x38, alpha_test_enable = 0x60, alpha_reference = 0x64,
    alpha_function = 0x68
};
struct NativeCullState {
    plume::RenderCullMode mode;
    bool front_counter_clockwise;
    bool operator==(const NativeCullState&) const = default;
};
// Unset fields must be supplied by original state calls before PSO/shader use.
struct NativeRenderState {
    std::optional<bool> alpha_test_enabled;
    std::optional<plume::RenderComparisonFunction> alpha_function;
    std::optional<float> alpha_reference;
    std::optional<bool> depth_enable_requested;
    std::optional<plume::RenderComparisonFunction> depth_function;
    std::optional<bool> depth_write_enabled;
    std::optional<NativeCullState> cull;
    // Reset index, topology and index format must come from the actual draw.
    std::optional<bool> primitive_restart_enabled;
    bool operator==(const NativeRenderState&) const = default;
};
}
