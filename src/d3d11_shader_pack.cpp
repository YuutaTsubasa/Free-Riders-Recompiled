#include "d3d11_shader_pack.h"
#include "shader_pack_format.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace sfr {
namespace {
constexpr char magic[8] = {'S', 'F', 'R', 'D', '3', 'D', '1', '1'};
// Bounds against a damaged file: no shader or DXBC is near these.
constexpr uint32_t max_shaders = 1u << 16, max_variants = 16, max_bytes = 16u << 20;

void put(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}
void put(std::vector<uint8_t>& out, std::span<const uint8_t> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

struct Reader {
    std::span<const uint8_t> bytes;
    size_t at = 0;
    uint32_t word() {
        if (bytes.size() - at < 4) throw std::invalid_argument("the D3D11 shader pack is truncated");
        const uint32_t value = uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
                               uint32_t(bytes[at + 3]) << 24;
        at += 4;
        return value;
    }
    std::vector<uint8_t> take(uint32_t size) {
        if (size > max_bytes || bytes.size() - at < size) throw std::invalid_argument("the D3D11 shader pack is truncated");
        std::vector<uint8_t> part(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + size));
        at += size;
        return part;
    }
};
}

std::vector<uint8_t> encode_d3d11_shader_pack(std::span<const D3D11PackedShader> shaders) {
    std::vector<uint8_t> out(std::begin(magic), std::end(magic));
    put(out, shader_abi_version);
    put(out, d3d11_shader_source_version);
    put(out, uint32_t(shaders.size()));
    for (const auto& shader : shaders) {
        put(out, shader.stage);
        put(out, shader.mask);
        put(out, uint32_t(shader.source.size()));
        put(out, uint32_t(shader.hlsl.size()));
        put(out, uint32_t(shader.variants.size()));
        put(out, shader.source);
        put(out, shader.hlsl);
        for (const auto& variant : shader.variants) {
            put(out, variant.spec);
            put(out, uint32_t(variant.dxbc.size()));
            put(out, variant.dxbc);
        }
    }
    return out;
}

std::vector<D3D11PackedShader> decode_d3d11_shader_pack(std::span<const uint8_t> bytes) {
    if (bytes.size() < 20 || std::memcmp(bytes.data(), magic, sizeof magic) != 0)
        throw std::invalid_argument("not a D3D11 shader pack");
    Reader in{bytes, sizeof magic};
    if (const uint32_t abi = in.word(); abi != shader_abi_version)
        throw std::invalid_argument("D3D11 shader pack ABI " + std::to_string(abi) + " is not " +
                                    std::to_string(shader_abi_version));
    if (const uint32_t version = in.word(); version != d3d11_shader_source_version)
        throw std::invalid_argument("D3D11 shader pack source version " + std::to_string(version) + " is not " +
                                    std::to_string(d3d11_shader_source_version));
    const uint32_t count = in.word();
    if (!count || count > max_shaders) throw std::invalid_argument("the D3D11 shader pack has no or too many shaders");
    std::vector<D3D11PackedShader> shaders(count);
    for (auto& shader : shaders) {
        shader.stage = in.word();
        shader.mask = in.word();
        const uint32_t source = in.word(), hlsl = in.word(), variants = in.word();
        if (shader.stage > 1 || !source || !hlsl || variants > max_variants)
            throw std::invalid_argument("the D3D11 shader pack has an invalid entry");
        shader.source = in.take(source);
        shader.hlsl = in.take(hlsl);
        shader.variants.resize(variants);
        for (auto& variant : shader.variants) {
            variant.spec = in.word();
            variant.dxbc = in.take(in.word());
            if (variant.dxbc.size() < 4 || std::memcmp(variant.dxbc.data(), "DXBC", 4) != 0)
                throw std::invalid_argument("the D3D11 shader pack has a variant that is not DXBC");
        }
    }
    if (in.at != bytes.size()) throw std::invalid_argument("the D3D11 shader pack has trailing data");
    return shaders;
}
}
