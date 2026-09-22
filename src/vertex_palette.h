#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sfr {
// Sonic Free Riders' skinned vertex shaders fetch bone matrices with vfetch
// instructions the vertex declaration does not describe: fully specified
// fetches (format, stride and offset in the instruction) from fetch constant
// 94, which SetStreamSource fills for stream 1. XenosRecomp only knows the
// fetches the declaration covers and asserts on the rest, so the container is
// patched to declare them as one unused input and the translated HLSL reads
// them from a constant buffer instead.
struct VertexPaletteFetch {
    uint32_t address;           // microcode instruction address
    uint32_t source_register;   // register holding the palette entry index
    uint32_t source_component;  // 0..3, the component within that register
    uint32_t row;               // float4 within the entry
};

struct VertexPalette {
    std::vector<uint8_t> container;           // container declaring the fetches
    std::vector<VertexPaletteFetch> fetches;  // in the translator's emission order
    uint32_t usage = 0, usage_index = 0;      // the input the declaration gives them
    uint32_t entry_float4s = 0;               // float4 per palette entry
};

// The palette a vertex shader container needs, or nothing when it needs none:
// pixel shaders, shaders whose declaration covers every fetch, and shaders
// whose fetches this does not understand (they stay untranslatable).
std::optional<VertexPalette> vertex_palette(std::span<const uint8_t> container);

// Rewrites translated HLSL: drops the input the patched declaration added and
// reads each of its fetches from the palette constant buffer. Returns nothing
// when the text does not hold exactly one read per fetch.
std::optional<std::string> apply_vertex_palette(const VertexPalette& palette, const std::string& hlsl);

// Entries the palette holds, given the byte size of the stream behind it.
uint32_t vertex_palette_entries(const VertexPalette& palette, uint32_t stream_bytes);
}
