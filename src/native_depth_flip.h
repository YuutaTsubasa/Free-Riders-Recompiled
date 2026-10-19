#pragma once
// Inverted viewport depth where the device cannot do it (Issue #177).
//
// The title draws with MinDepth > MaxDepth (1 and 0): depth = MinDepth +
// (MaxDepth - MinDepth) * z/w. Vulkan allows that; D3D12 only where the
// driver reports InvertedViewportDepthFlipsZSupported (OPTIONS13), which
// older drivers (an MX110 on 457.49, Issue #177) do not. There the viewport
// gets its range in order, [MaxDepth, MinDepth], and a geometry shader
// (shaders/depth_flip.hlsl) turns clip-space z into w - z:
//   MaxDepth + (MinDepth - MaxDepth) * (w - z)/w = MinDepth + (MaxDepth - MinDepth) * z/w,
// the depth the inverted viewport stores. Everything else (comparisons,
// clears, sampled depth, SV_Position.z) is left as the title set it.
#include "plume_render_interface_types.h"

#include <utility>

namespace sfr {
// Whether a draw under this viewport needs its z flipped by the shader.
inline bool depth_flip_needed(const plume::RenderViewport& viewport, bool device_inverts_depth) {
    return !device_inverts_depth && viewport.minDepth > viewport.maxDepth;
}

// The viewport given to the device: the title's own, or (flipped) the same
// one with its depth range in order.
inline plume::RenderViewport depth_flip_viewport(plume::RenderViewport viewport, bool device_inverts_depth) {
    if (depth_flip_needed(viewport, device_inverts_depth)) std::swap(viewport.minDepth, viewport.maxDepth);
    return viewport;
}

// The geometry shader takes the primitive the input assembler makes.
enum class DepthFlipStage { none, point, line, triangle };
inline DepthFlipStage depth_flip_stage(plume::RenderPrimitiveTopology topology) {
    using Topology = plume::RenderPrimitiveTopology;
    switch (topology) {
    case Topology::POINT_LIST: return DepthFlipStage::point;
    case Topology::LINE_LIST:
    case Topology::LINE_STRIP: return DepthFlipStage::line;
    case Topology::TRIANGLE_LIST:
    case Topology::TRIANGLE_STRIP:
    case Topology::TRIANGLE_FAN: return DepthFlipStage::triangle;
    default: return DepthFlipStage::none;
    }
}

// SFR_TEST_NO_INVERTED_DEPTH=1 takes the flipped path on a device that has
// inverted depth, so it can be compared with the device's own.
inline bool inverted_depth_disabled_for_test(const char* setting) {
    return setting && setting[0] == '1' && setting[1] == 0;
}
}
