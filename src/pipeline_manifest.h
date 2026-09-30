#pragma once
#include "native_renderer.h"
#include <filesystem>

namespace sfr {
inline constexpr size_t pipeline_manifest_max_bytes = 16 * 1024 * 1024;
inline constexpr size_t pipeline_manifest_max_recipes = 4096;
inline constexpr size_t pipeline_manifest_max_elements = 32;

struct PipelineShaderId {
    uint64_t hash = 0;
    uint32_t size = 0;
    bool operator==(const PipelineShaderId&) const = default;
};
PipelineShaderId pipeline_shader_id(std::span<const uint8_t> source);

struct PipelineRecipe {
    PipelineShaderId vertex, pixel;
    uint32_t pixel_link_constants = 0;
    // Only pipeline fields are encoded. Shader pointers, spans and per-draw
    // constants are ignored and remain default-initialized when decoded.
    NativeDraw state;
};

// All encodings use fixed-width little-endian fields, no pointers or padding.
// Invalid or unsupported state throws invalid_argument. Semantic strings in
// decoded recipes have static storage duration, independent of the input file.
std::vector<uint8_t> pipeline_recipe_key(const PipelineRecipe& recipe);
// backend: 0 = D3D12, 1 = Vulkan. Duplicate recipes are removed, preserving
// first-seen order. Decoder rejects stale ABI/backend, corruption and excesses.
std::vector<uint8_t> encode_pipeline_manifest(std::span<const PipelineRecipe> recipes, uint32_t backend);
std::vector<PipelineRecipe> decode_pipeline_manifest(std::span<const uint8_t> bytes, uint32_t backend);
// Missing files return empty. Other read/format failures throw invalid_argument.
std::vector<PipelineRecipe> load_pipeline_manifest_file(const std::filesystem::path& path, uint32_t backend);
// Replaces the destination atomically only after writing the complete file.
// I/O failures return false; invalid recipe arguments throw invalid_argument.
bool save_pipeline_manifest_file(const std::filesystem::path& path,
                                 std::span<const PipelineRecipe> recipes, uint32_t backend);
}
