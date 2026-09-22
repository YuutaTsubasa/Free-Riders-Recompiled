#include "vertex_palette.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void put_be32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    if (data.size() < offset + 4) data.resize(offset + 4);
    for (int b = 0; b < 4; ++b) data[offset + b] = uint8_t(value >> (24 - 8 * b));
}

uint32_t get_be32(const std::vector<uint8_t>& data, size_t offset) {
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 | uint32_t(data[offset + 2]) << 8 |
           data[offset + 3];
}

// A vertex shader container: one exec of two fetches, the first declared as
// TEXCOORD0 and the second a float4 row of a 64-byte entry from stream 1.
constexpr uint32_t shader_at = 0x24, virtual_size = 128, code_at = virtual_size;

std::vector<uint8_t> container(uint32_t palette_word0, uint32_t palette_word1, uint32_t palette_word2) {
    std::vector<uint8_t> data(virtual_size, 0);
    put_be32(data, 0, 1);            // flags: a vertex shader
    put_be32(data, 4, virtual_size);
    put_be32(data, 0x18, shader_at);
    put_be32(data, shader_at + 0, 0);   // physical offset
    put_be32(data, shader_at + 4, 36);  // microcode size in bytes
    put_be32(data, shader_at + 0x14, 0);  // no interpolators
    put_be32(data, shader_at + 0x18, 0);  // no prefix words
    put_be32(data, shader_at + 0x1C, 1);  // one declared element
    put_be32(data, shader_at + 0x24, 1 | 5 << 12 | 0 << 16);  // address 1, TEXCOORD0
    // Exec from address 1, two instructions, both fetches.
    put_be32(data, code_at + 0, 1 | 2 << 12 | 0x5 << 16);
    put_be32(data, code_at + 4, 1 << 12);  // opcode 1 (Exec) in bits 44..47
    put_be32(data, code_at + 8, 0);
    put_be32(data, code_at + 12, 0x45F84000);  // declared fetch into r4
    put_be32(data, code_at + 16, 0x688);
    put_be32(data, code_at + 20, 0);
    put_be32(data, code_at + 24, palette_word0);
    put_be32(data, code_at + 28, palette_word1);
    put_be32(data, code_at + 32, palette_word2);
    return data;
}

constexpr uint32_t palette_fetch = 3 << 5 | 5 << 12 | 1 << 19 | 31 << 20 | 1 << 25 | 2u << 30;  // r3.z, constant 94
constexpr uint32_t palette_format = 38 << 16 | 0x688;
constexpr uint32_t palette_offset = 16 | 4 << 8;  // 16 dword stride, row 1

const std::string hlsl =
    "struct VertexShaderInput\n"
    "{\n"
    "#ifdef __air__\n"
    "\tfloat4 iTexCoord0 [[attribute(13)]];\n"
    "\tfloat4 iPosition3 [[attribute(3)]];\n"
    "#else\n"
    "\t[[vk::location(13)]] float4 iTexCoord0 : TEXCOORD0;\n"
    "\t[[vk::location(3)]] float4 iPosition3 : POSITION3;\n"
    "#endif\n"
    "};\n"
    "\tr4.xyzw = (float4)(swapFloats(g_SwappedTexcoords, (input.iTexCoord0), 0)).xyzw;\n"
    "\tr5.xyzw = (float4)((input.iPosition3)).xyzw;\n";

void run() {
    const auto source = container(palette_fetch, palette_format, palette_offset);
    require(!sfr::vertex_palette(container(0x45F84000, 0x688, 0)),
            "an undeclared fetch from another constant is not a palette");

    const auto patched = sfr::vertex_palette(source);
    require(patched.has_value(), "the undeclared stream 1 fetch makes a palette");
    require(patched->fetches.size() == 1, "one palette fetch");
    const auto& fetch = patched->fetches.front();
    require(fetch.address == 2 && fetch.source_register == 3 && fetch.source_component == 2 && fetch.row == 1,
            "the fetch reads row 1 by the index in r3.z");
    require(patched->entry_float4s == 4, "a 64-byte entry holds four float4");
    require(patched->usage == 0 && patched->usage_index == 3, "the free POSITION3 input carries it");

    // The patched container declares the fetch without moving the microcode.
    const auto& data = patched->container;
    const uint32_t shader = get_be32(data, 0x18);
    require(shader == source.size(), "the patched shader is appended");
    require(get_be32(data, shader + 4) == 36 && get_be32(data, shader) == 0, "the microcode keeps its place");
    require(get_be32(data, shader + 0x1C) == 2, "two elements are declared");
    require(get_be32(data, shader + 0x24) == (1 | 5 << 12), "the declared element is kept");
    require(get_be32(data, shader + 0x28) == (2 | 0 << 12 | 3 << 16), "the fetch is declared as POSITION3");
    for (size_t i = 0; i < source.size(); ++i)
        require(i == 0x18 + 3 || data[i] == source[i], "only the shader offset changes");

    const auto text = sfr::apply_vertex_palette(*patched, hlsl);
    require(text.has_value(), "the read is rewritten");
    require(text->find("iPosition3 ") == std::string::npos && text->find(": POSITION3") == std::string::npos,
            "the input the patch added is gone");
    require(text->find("input.iTexCoord0") != std::string::npos, "declared inputs are untouched");
    require(text->find("r5.xyzw = (float4)((sfrVertexPalette(int(r3.z) * 4 + 1))).xyzw;") != std::string::npos,
            "the palette read replaces it");
    require(text->find("cbuffer VertexPalette : register(b3, space4)") < text->find("struct VertexShaderInput"),
            "the palette buffer is declared first");

    auto extra = *patched;
    extra.fetches.push_back(fetch);
    require(!sfr::apply_vertex_palette(extra, hlsl), "a read count that differs is refused");

    require(sfr::vertex_palette_entries(*patched, 64 * 10) == 10, "entries follow the stream size");
    require(sfr::vertex_palette_entries(*patched, 64 * 4096) == 256, "the constant buffer caps them");
}
}

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << "vertex_palette_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "vertex_palette_test: ok\n";
    return 0;
}
