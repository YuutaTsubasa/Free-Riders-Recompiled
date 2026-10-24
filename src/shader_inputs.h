#pragma once
#include <optional>
#include <string>

namespace sfr {
// A shader container declares one vertex element per vertex fetch instruction,
// so a shader that fetches the same attribute from several places declares it
// several times and the translated input structure repeats the member. Drops
// the repeats, each of the structure's preprocessor branches on its own, and
// returns nothing when there are none.
std::optional<std::string> dedupe_vertex_inputs(const std::string& hlsl);
// D3D12-only paired vertex library; nullopt rejects an unrecognized source.
std::optional<std::string> dec3n_vertex_shader_source(const std::string& legacyHlsl);
}
