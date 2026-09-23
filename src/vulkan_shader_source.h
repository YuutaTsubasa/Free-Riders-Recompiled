#pragma once
#include <optional>
#include <string>

namespace sfr {
// A translated shader's HLSL (with the common header inlined) made fit to
// compile to SPIR-V: g_ScreenSpaceScale gets its definition in the SPIR-V
// branch of the header (shared constants + 320), and the palette and loop
// constant buffers this project adds are read through push-constant
// addresses of their own. Null when the text lacks the header's SPIR-V anchor.
std::optional<std::string> vulkan_shader_source(const std::string& hlsl);
}
