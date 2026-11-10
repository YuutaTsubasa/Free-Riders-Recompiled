// shaders-d3d11.pack's format (src/d3d11_shader_pack.h): what is written
// reads back the same, and a damaged or outdated pack is refused (the game
// then leaves it out). Synthetic bytes only; no game shader.
#include "d3d11_shader_pack.h"
#include "shader_pack_format.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool refused(std::vector<uint8_t> bytes) {
    try {
        sfr::decode_d3d11_shader_pack(bytes);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
std::vector<uint8_t> dxbc(uint8_t fill) { return {'D', 'X', 'B', 'C', fill, fill, fill}; }
}

int main() {
    std::vector<sfr::D3D11PackedShader> shaders(2);
    shaders[0].stage = 0;
    shaders[0].source = {1, 2, 3, 4};
    shaders[0].hlsl = {'v', 's'};
    shaders[0].variants = {{0, dxbc(7)}};
    shaders[1].stage = 1;
    shaders[1].mask = 2;
    shaders[1].source = {5, 6, 7};
    shaders[1].hlsl = {'p', 's', '!'};
    shaders[1].variants = {{0, dxbc(8)}, {2, dxbc(9)}};
    const auto bytes = sfr::encode_d3d11_shader_pack(shaders);
    const auto back = sfr::decode_d3d11_shader_pack(bytes);
    expect(back.size() == 2, "two shaders read back");
    expect(back[1].stage == 1 && back[1].mask == 2, "stage and mask read back");
    expect(back[1].source == shaders[1].source && back[1].hlsl == shaders[1].hlsl, "source and HLSL read back");
    expect(back[1].variants.size() == 2 && back[1].variants[1].spec == 2 && back[1].variants[1].dxbc == dxbc(9),
           "variants read back");

    auto truncated = bytes;
    truncated.pop_back();
    expect(refused(truncated), "a truncated pack is refused");
    auto trailing = bytes;
    trailing.push_back(0);
    expect(refused(trailing), "trailing data is refused");
    auto abi = bytes;
    abi[8] ^= 1;
    expect(refused(abi), "another shader ABI is refused");
    auto version = bytes;
    version[12] ^= 1;
    expect(refused(version), "another D3D11 source version is refused");
    auto magic = bytes;
    magic[0] = 'X';
    expect(refused(magic), "a file that is not a D3D11 pack is refused");
    shaders[0].variants[0].dxbc = {1, 2, 3, 4};
    expect(refused(sfr::encode_d3d11_shader_pack(shaders)), "a variant that is not DXBC is refused");
    expect(refused({}), "an empty file is refused");

    if (failures) return EXIT_FAILURE;
    std::puts("d3d11 shader pack: ok");
    return EXIT_SUCCESS;
}
