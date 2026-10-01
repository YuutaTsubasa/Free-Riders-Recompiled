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
    require(text->find("#define g_ScreenSpaceScale (sfrRawBufferLoad<float2, 8>(g_PushConstants.SharedConstants, sfrOffset(320)))") !=
                std::string::npos, "the screen scale reads shared constant bytes 320");
    require(text->find("#define g_ScreenSpaceScale") < text->find("#else"), "only the SPIR-V branch gains it");
    require(text->find("#define g_ResolvedTextureScale (sfrRawBufferLoad<float2, 8>(g_PushConstants.SharedConstants, sfrOffset(328)))") !=
                std::string::npos, "resolved texture scale reads the aligned shared constant extension");
    require(!sfr::vulkan_shader_source("float4 x;\n"), "text without the header is refused");
    std::string crlf;
    for (const char c : header) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    require(sfr::vulkan_shader_source(crlf).has_value(), "CRLF line ends are accepted");
}

void reads_the_palette_and_loop_constants_through_addresses() {
    const std::string body =
        "cbuffer VertexPalette : register(b3, space4)\n{\n\tfloat4 g_VertexPalette[1024];\n};\n\n"
        "float4 sfrVertexPalette(int index)\n{\n\treturn g_VertexPalette[uint(index) & 1023u];\n}\n\n"
        "cbuffer LoopConstants : register(b4, space4)\n{\n\tint4 g_LoopConstants[16];\n};\n\n"
        "\tint4 i0 = g_LoopConstants[0];\n\tint4 i3 = g_LoopConstants[3];\n\tint4 x = my_g_LoopConstants[1];\n";
    const auto text = sfr::vulkan_shader_source(header + body);
    require(text.has_value(), "a shader with both buffers converts");
    require(text->find("cbuffer VertexPalette") == std::string::npos && text->find("cbuffer LoopConstants") == std::string::npos,
            "the root constant buffers are gone");
    require(text->find("return sfrRawBufferLoad<float4, 16>(g_PushConstants.VertexPalette, "
                       "sfrScale16(sfrOffset(uint(index) & 1023u)));") != std::string::npos,
            "a palette read loads from the palette push constant");
    require(text->find("int4 i3 = sfrRawBufferLoad<int4, 16>(g_PushConstants.LoopConstants, "
                       "sfrScale16(sfrOffset(3)));") != std::string::npos,
            "a loop constant loads from the loop push constant");
    require(text->find("my_g_LoopConstants[1]") != std::string::npos, "a longer name is left alone");
}

void loads_push_constant_addresses_as_32_bit_pairs() {
    const auto text = sfr::vulkan_shader_source(header +
        "float4 c = vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + (3 + min(INDEX, 220)) * 16, 16);\n");
    require(text.has_value(), "the complete push constant header converts");
    require(text->find("uint64_t") == std::string::npos,
            "Vulkan buffer addresses must not require shaderInt64 on Adreno");
    for (const char* field : {"VertexShaderConstants", "PixelShaderConstants", "SharedConstants",
                              "VertexPalette", "LoopConstants"}) {
        require(text->find(std::string("uint2 ") + field + ";") != std::string::npos,
                "each push constant address is loaded as two 32-bit words");
        require(text->find(std::string("uint64_t ") + field + ";") == std::string::npos,
                "no 64-bit push constant load remains");
    }
    require(text->find("sfrPointerCast<T>(sfrAddAddress(address, offset))") != std::string::npos,
            "physical pointers consume both address words");
    require(text->find("sfrOffset((3 + min(INDEX, 220)) * 16)") != std::string::npos,
            "nested offset expressions keep their original type and arithmetic");
    require(text->find("vk::RawBufferLoad<") == std::string::npos, "all raw loads use Int64-free physical pointers");
}

void preserves_load_types_and_rejects_unknown_shapes() {
    for (const char* type : {"uint", "uint2", "uint3", "uint4", "int", "int2", "int3", "int4",
                             "float", "float2", "float3", "float4"}) {
        const auto text = sfr::vulkan_shader_source(header + std::string("x = vk::RawBufferLoad<") +
            type + ">(g_PushConstants.SharedConstants + unsignedOffset, 0x10);");
        require(text && text->find(std::string("sfrRawBufferLoad<") + type + ", 16>") != std::string::npos,
                "scalar and vector types retain their type/alignment");
        require(text->find("sfrOffset(unsignedOffset)") != std::string::npos,
                "unsigned offsets are not forced into signed int");
    }
    const auto boolean = sfr::vulkan_shader_source(header +
        "b = vk::RawBufferLoad<bool>(g_PushConstants.SharedConstants + -4);\n"
        "b = (swappedFloats & (1ull << semanticIndex)) != 0;\n");
    require(boolean && boolean->find("(sfrRawBufferLoad<uint, 4>(g_PushConstants.SharedConstants, sfrOffset(-4))) != 0u")
                != std::string::npos, "bool loads retain 32-bit memory representation and signed offsets");
    require(boolean->find("1ull") == std::string::npos && boolean->find("(semanticIndex & 63u) < 32u") != std::string::npos,
            "32-bit bitmasks preserve the upper half of 64-bit shift results");
    for (const char* bad : {
             "vk::RawBufferLoad<float4>(unknown + 4)",
             "vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 4)",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + uint64_t(index))",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4, 3)",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4, 4, 8)",
             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + (4)",
             "g_VertexPalette[index"})
        require(!sfr::vulkan_shader_source(header + bad), "unrecognized physical loads fail closed");
    require(!sfr::vulkan_shader_source(header.substr(push.size())), "incomplete push constant header is refused");
    std::string missing = header;
    missing.erase(missing.find("uint64_t LoopConstants;"), std::string("uint64_t LoopConstants;").size());
    require(!sfr::vulkan_shader_source(missing), "every address member is required");
}
}

int main() {
    try {
        defines_the_screen_scale();
        reads_the_palette_and_loop_constants_through_addresses();
        loads_push_constant_addresses_as_32_bit_pairs();
        preserves_load_types_and_rejects_unknown_shapes();
        std::cout << "Vulkan shader source checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
