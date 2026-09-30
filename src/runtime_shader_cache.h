#pragma once
#include "shader_cache.h"
#include <span>

namespace sfr {
// Shaders the prepared cache lacks (the game loads most of them from its
// asset archives) are translated when first created: the pinned XenosRecomp
// translator writes HLSL and DXC compiles it exactly as
// scripts/prepare_shaders.py does. Results persist in a disk cache keyed by
// the container bytes, so later runs only read them back.
//
// Environment overrides: SFR_SHADER_TRANSLATOR, SFR_SHADER_COMMON, SFR_DXC
// and SFR_RUNTIME_SHADER_CACHE (directory). The returned entry stays valid for
// the process lifetime.
const ShaderCacheEntry& runtime_shader(ShaderStage stage, std::span<const uint8_t> source);
// Resolves an optional pipeline recipe without invoking the runtime translator.
// Hash collisions/ambiguous identities and absent payloads return nullptr.
const ShaderCacheEntry* packed_pipeline_shader(ShaderStage stage, uint64_t hash, uint32_t source_size);
// Off by default (unit tests keep the prepared-cache contract); the
// diagnostic enables it.
extern bool runtime_shader_translation;
}
