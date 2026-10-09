#include "vulkan_shader_source.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const std::string push =
    "struct PushConstants {\n"
    "    uint64_t VertexShaderConstants;\n"
    "    uint64_t PixelShaderConstants;\n"
    "    uint64_t SharedConstants;\n"
    "    uint64_t VertexPalette;\n"
    "    uint64_t LoopConstants;\n};\n"
    "[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;\n";
const std::string header = push +
    "#ifdef __spirv__\n"
    "#define g_conditionalRenderingIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 316)\n"
    "#else\n"
    "    float2 g_ScreenSpaceScale : packoffset(c20.x);\n"
    "#endif\n";

void defines_the_screen_scale() {
    const auto text = sfr::vulkan_shader_source(header + "float2 s = g_ScreenSpaceScale;\n");
    require(text.has_value(), "the header's SPIR-V branch is found");
    require(text->find("#define g_ScreenSpaceScale (asfloat(sfrWords2SC(uint(320))))") != std::string::npos,
            "the screen scale reads shared constant bytes 320");
    require(text->find("#define g_ScreenSpaceScale") < text->find("#else"), "only the SPIR-V branch gains it");
    require(text->find("#define g_ResolvedTextureScale (asfloat(sfrWords2SC(uint(328))))") != std::string::npos,
            "resolved texture scale reads the shared constant extension");
    require(!sfr::vulkan_shader_source("float4 x;\n"), "text without the header is refused");
    std::string crlf;
    for (const char c : header) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    require(sfr::vulkan_shader_source(crlf).has_value(), "CRLF line ends are accepted");
}

void declares_the_uniform_buffers() {
    const auto text = sfr::vulkan_shader_source(header);
    require(text.has_value(), "the header converts");
    for (const char* buffer : {"[[vk::binding(0, 2)]] cbuffer SfrVertexShaderConstants { uint4 sfrRowsVS[256]; };",
                               "[[vk::binding(1, 2)]] cbuffer SfrPixelShaderConstants { uint4 sfrRowsPS[256]; };",
                               "[[vk::binding(2, 2)]] cbuffer SfrSharedConstants { uint4 sfrRowsSC[32]; };"})
        require(text->find(buffer) != std::string::npos, "each shader constant block is a uniform buffer in set 2");
    require(text->find("uint64_t") == std::string::npos, "no shader asks for shaderInt64");
    for (const char* field : {"VertexShaderConstants", "PixelShaderConstants", "SharedConstants",
                              "VertexPalette", "LoopConstants"})
        require(text->find(std::string("uint2 ") + field + ";") != std::string::npos,
                "the unread push constants keep their 40-byte layout without 64-bit members");
}

void binds_the_palette_loop_constants_textures_and_samplers() {
    const std::string body =
        "Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);\n"
        "Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[] : register(t0, space1);\n"
        "TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);\n"
        "SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);\n"
        "cbuffer VertexPalette : register(b3, space4)\n{\n\tfloat4 g_VertexPalette[1024];\n};\n\n"
        "cbuffer LoopConstants : register(b4, space4)\n{\n\tint4 g_LoopConstants[16];\n};\n\n"
        "\tint4 i3 = g_LoopConstants[3];\n";
    const auto text = sfr::vulkan_shader_source(header + body);
    require(text.has_value(), "a shader with every resource converts");
    require(text->find("[[vk::binding(3, 2)]] cbuffer VertexPalette : register(b3, space4)") != std::string::npos,
            "the palette is uniform buffer 3 of set 2");
    require(text->find("[[vk::binding(4, 2)]] cbuffer LoopConstants : register(b4, space4)") != std::string::npos,
            "the loop constants are uniform buffer 4 of set 2");
    require(text->find("int4 i3 = g_LoopConstants[3];") != std::string::npos, "their reads are left as they are");
    for (const char* heap : {"[[vk::binding(0, 0)]] Texture2D<float4> g_Texture2DDescriptorHeap[]",
                             "[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[]",
                             "[[vk::binding(0, 0)]] TextureCube<float4> g_TextureCubeDescriptorHeap[]"})
        require(text->find(heap) != std::string::npos, "the three texture heaps share set 0");
    require(text->find("[[vk::binding(0, 1)]] SamplerState g_SamplerDescriptorHeap[]") != std::string::npos,
            "the samplers are set 1");
}

void reads_constants_by_type_and_alignment() {
    const auto text = sfr::vulkan_shader_source(header +
        "float4 c = vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + (3 + min(INDEX, 220)) * 16, 16);\n"
        "float4 v = vk::RawBufferLoad<float4>(g_PushConstants.VertexShaderConstants + 32);\n"
        "uint u = vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256);\n"
        "int3 i = vk::RawBufferLoad<int3>(g_PushConstants.VertexShaderConstants + 48, 16);\n"
        "bool b = vk::RawBufferLoad<bool>(g_PushConstants.SharedConstants + 304);\n");
    require(text.has_value(), "the loads convert");
    require(text->find("float4 c = (asfloat(sfrRowPS(uint((3 + min(INDEX, 220)) * 16))));") != std::string::npos,
            "an aligned float4 reads a whole row, keeping the offset expression");
    require(text->find("float4 v = (asfloat(sfrWords4VS(uint(32))));") != std::string::npos,
            "a float4 of unstated alignment is gathered word by word");
    require(text->find("uint u = (sfrWordSC(uint(256)));") != std::string::npos, "a scalar reads one word");
    require(text->find("int3 i = (asint(sfrWords3VS(uint(48))));") != std::string::npos, "an int3 reads three words");
    require(text->find("bool b = ((sfrWordSC(uint(304)) != 0u));") != std::string::npos,
            "a bool keeps its 32-bit representation");
    require(text->find("vk::RawBufferLoad<") == std::string::npos, "no buffer-address load remains");
}

void rejects_unknown_shapes() {
    const auto mask = sfr::vulkan_shader_source(header + "b = (swappedFloats & (1ull << semanticIndex)) != 0;\n");
    require(mask && mask->find("1ull") == std::string::npos && mask->find("(semanticIndex & 63u) < 32u") != std::string::npos,
            "32-bit bitmasks preserve the upper half of 64-bit shift results");
    for (const char* bad : {
             "vk::RawBufferLoad<float4>(unknown + 4)",
             "vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 4)",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + uint64_t(index))",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4, 3)",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4, 4, 8)",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + (4)",
             "vk::RawBufferLoad<float4>(g_PushConstants.VertexPalette + 16)",
             "vk::RawBufferLoad<bool2>(g_PushConstants.SharedConstants + 4)"})
        require(!sfr::vulkan_shader_source(header + bad), "unrecognized loads fail closed");
    require(!sfr::vulkan_shader_source(header.substr(push.size())), "incomplete push constant header is refused");
    std::string missing = header;
    missing.erase(missing.find("uint64_t LoopConstants;"), std::string("uint64_t LoopConstants;").size());
    require(!sfr::vulkan_shader_source(missing), "every address member is required");
}
}

int main() {
    try {
        defines_the_screen_scale();
        declares_the_uniform_buffers();
        binds_the_palette_loop_constants_textures_and_samplers();
        reads_constants_by_type_and_alignment();
        rejects_unknown_shapes();
        std::cout << "Vulkan shader source checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
