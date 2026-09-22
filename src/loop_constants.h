#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace sfr {
// Loop constants are the integer registers a shader's loops count with. The
// translator declares only the ones the container embeds, so a shader whose
// counts come from SetVertexShaderConstantI refers to an undeclared i<N>.
// The title keeps those registers in the device shadow, and the draw uploads
// them as a constant buffer the declaration below reads.
constexpr uint32_t loop_constants = 16;

// Declares every loop constant the translated HLSL uses but does not define.
// Returns nothing when it defines them all, and refuses a pixel shader (only
// the vertex registers are uploaded).
std::optional<std::string> add_loop_constants(const std::string& hlsl);
}
