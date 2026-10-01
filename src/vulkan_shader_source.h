#pragma once
#include <optional>
#include <string>

namespace sfr {
// A translated shader's HLSL (with the common header inlined) made fit to
// compile to SPIR-V: g_ScreenSpaceScale gets its definition in the SPIR-V
// branch of the header (shared constants + 320), and the palette and loop
// constant buffers this project adds are read through push-constant
// addresses of their own. Physical pointer arithmetic uses 32-bit pairs, so
// Vulkan does not require shaderInt64. Null for an incomplete header or an
// unsupported physical load, rather than emitting a truncated GPU address.
std::optional<std::string> vulkan_shader_source(const std::string& hlsl);
}
