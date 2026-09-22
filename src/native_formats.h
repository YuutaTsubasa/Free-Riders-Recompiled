#pragma once
#include "plume_render_interface_types.h"
#include "texture_fetch.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sfr {
// Xbox 360 D3DDECLUSAGE values and the D3D12 semantic of each.
const char* declaration_semantic(uint32_t usage);

// The pinned XenosRecomp maps TEXCOORD0-3 only. Vertex shaders declaring
// TEXCOORD4-12 are translated with those elements renamed to the names it does
// map and the game does not use (see runtime_shader_cache.cpp); draws bind them
// under the same names. Every other (usage, index) is returned unchanged.
struct ShaderInputUsage { uint32_t usage, index; };
constexpr ShaderInputUsage shader_input_usage(uint32_t usage, uint32_t index) {
    if (usage != 5 || index < 4 || index > 12) return {usage, index};
    constexpr ShaderInputUsage names[] = {{0, 1}, {0, 2}, {0, 3}, {3, 1}, {3, 2},
                                          {3, 3}, {6, 1}, {6, 2}, {6, 3}};
    return names[index - 4];
}

struct DeclarationFormat {
    plume::RenderFormat format;
    // 16-bit component pairs arrive swapped after the 32-bit vertex word swap;
    // XenosRecomp shaders unswap them when the usage's g_Swapped* bit is set.
    bool swapped_pairs;
};
// Xbox 360 D3DDECLTYPE (e.g. FLOAT3 0x2A23B9); empty when unsupported.
std::optional<DeclarationFormat> declaration_format(uint32_t type);

// Guest vertex data is big-endian 32-bit words; the host reads little-endian.
void swap_words(std::span<uint8_t> bytes);
// swap_words from one buffer into another of the same size: one pass where a
// copy and an in-place swap took two. The destination may be write-combined
// (the renderer's upload ring); it is only written.
void swap_words_into(std::span<uint8_t> to, std::span<const uint8_t> from);
// Decodes count big-endian indices (two or four bytes each) into out as
// base + index. A primitive-restart index (all ones, when restart is on)
// is written as it is and left out of lowest/highest, which cover the
// others (lowest > highest when there are none).
struct IndexScan {
    uint32_t lowest = ~0u, highest = 0;
    bool restart = false;
};
IndexScan decode_indices(const uint8_t* bytes, uint32_t count, bool wide, uint32_t base,
                         bool restart_enabled, uint32_t* out);

// Xenos texture endian modes: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
void swap_texture_bytes(std::span<uint8_t> bytes, uint32_t endian);

struct TextureLayout {
    plume::RenderFormat format;
    uint32_t block_width;      // texels per block edge (4 for DXT, 1 otherwise)
    uint32_t block_bytes;      // bytes per block (or per texel)
    uint32_t row_bytes;        // guest bytes per row of blocks, from the fetch pitch
    uint32_t rows;             // rows of blocks in the base level
    uint32_t width, height;
    bool tiled = false;        // Xenos 2D tiling: 32x32-block tiles
    uint32_t guest_rows = 0;   // rows of blocks the guest stores (32-aligned when tiled)
};

// Block index of block (x, y) in a tiled 2D surface pitch_blocks wide (a
// multiple of 32) with block_bytes per block: XGAddress2DTiledOffset.
uint32_t tiled_block_index(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t block_bytes);
// Rearranges a tiled base level into rows of layout.row_bytes.
std::vector<uint8_t> untile_texture(std::span<const uint8_t> tiled, const TextureLayout& layout);
// Base-level layout of a linear 2D texture; empty when the format is unsupported.
std::optional<TextureLayout> linear_texture_layout(const TextureFetch& fetch);

// SFR_ALLOW_RENDER_TARGETS=1 (investigation): depth textures (format 23) are
// laid out as four-byte texels so that a race which samples its shadow maps
// keeps rendering. Their content is not what the title resolved into them,
// because the native backend has no offscreen targets.
extern bool depth_texture_placeholder;

// Sampler state from fetch words: clamps from w0, filters from w3.
plume::RenderSamplerDesc fetch_sampler(const FetchWords& words);
}
