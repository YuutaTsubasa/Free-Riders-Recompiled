#pragma once
// shaders-d3d11.pack (Issue #359): the Direct3D 11 backend's shaders made
// ahead of time, beside shaders.pack, so that a player's first run neither
// translates nor compiles the shaders it holds. Built from a release's
// shaders.pack by sfr_d3d11_shader_pack (tools/d3d11_shader_pack.cpp) on
// Windows; like shaders.pack it holds code taken from the game, so it is
// private output and a release file, never committed.
//
// Per shader: its source container (the identity the game looks it up by),
// stage, specialization mask, the shader model 5.0 HLSL the D3D11 backend
// would have made of it (d3d11_shader_source.h), and DXBC for the
// specializations pipelines use (vertex 0; pixel 0 and the alpha test's 2).
//
// Format, little-endian: "SFRD3D11", u32 shader ABI, u32 D3D11 source
// version, u32 count, then per shader u32 stage (0 vertex, 1 pixel), mask,
// source size, HLSL size, variant count, the source and HLSL bytes, and per
// variant u32 specialization, DXBC size and the DXBC.
//
// The pack is optional: one that is missing, from another ABI or source
// version, or damaged is ignored (the backend translates and compiles as
// without it).
#include <cstdint>
#include <span>
#include <vector>

namespace sfr {
// Raised whenever the D3D11 rewrite (d3d11_shader_source.cpp) or the
// backend's bindings change what the HLSL must be: an older pack is then
// ignored rather than used.
inline constexpr uint32_t d3d11_shader_source_version = 1;

struct D3D11PackedShader {
    uint32_t stage = 0;  // 0 vertex, 1 pixel
    uint32_t mask = 0;
    std::vector<uint8_t> source, hlsl;
    struct Variant {
        uint32_t spec = 0;
        std::vector<uint8_t> dxbc;
    };
    std::vector<Variant> variants;
};

std::vector<uint8_t> encode_d3d11_shader_pack(std::span<const D3D11PackedShader> shaders);
// Throws std::invalid_argument naming what is wrong.
std::vector<D3D11PackedShader> decode_d3d11_shader_pack(std::span<const uint8_t> bytes);
}
