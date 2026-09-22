#pragma once
#include <cstdint>
#include <span>

namespace sfr {
enum class ShaderStage : uint32_t { vertex, pixel };
struct ShaderCacheEntry {
    std::span<const uint8_t> source;
    std::span<const uint8_t> dxil;
    ShaderStage stage;
    uint32_t specialization_mask;
    // float4 per entry of the skinning palette the vertex shader fetches from
    // stream 1 (see vertex_palette.h), or zero when it fetches none.
    uint32_t vertex_palette_float4s = 0;
    // The Vulkan bytecode (runtime cache only; last so the generated cache's
    // positional initializers stay valid).
    std::span<const uint8_t> spirv = {};
    // The bytecode for the backend in use; empty for a shader the pinned
    // translator rejected.
    std::span<const uint8_t> code(bool vulkan) const { return vulkan ? spirv : dxil; }
};
// Defined by the private, reproducibly generated cache (or the empty fallback).
std::span<const ShaderCacheEntry> compiled_shader_cache();
}
