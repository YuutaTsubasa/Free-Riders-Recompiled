#pragma once
#include <optional>
#include <string>

namespace sfr {
// Byte offsets in the shared constants (SharedConstants) that only the
// Vulkan shaders read: the buffer addresses of the skinning palette and of
// the loop constants, which D3D12 binds as root constant buffers b3 and b4.
constexpr unsigned vulkan_palette_address_offset = 328, vulkan_loop_address_offset = 336;

// A translated shader's HLSL (with the common header inlined) made fit to
// compile to SPIR-V: g_ScreenSpaceScale gets its definition in the SPIR-V
// branch of the header (shared constants + 320), and the palette and loop
// constant buffers this project adds are read through the addresses above
// instead. Null when the text lacks the header's SPIR-V anchor.
std::optional<std::string> vulkan_shader_source(const std::string& hlsl);
}
