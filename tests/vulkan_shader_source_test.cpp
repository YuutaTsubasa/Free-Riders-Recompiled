#include "vulkan_shader_source.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const std::string header =
    "#ifdef __spirv__\n"
    "#define g_conditionalRenderingIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 316)\n"
    "#else\n"
    "    float2 g_ScreenSpaceScale : packoffset(c20.x);\n"
    "#endif\n";

void defines_the_screen_scale() {
    const auto text = sfr::vulkan_shader_source(header + "float2 s = g_ScreenSpaceScale;\n");
    require(text.has_value(), "the header's SPIR-V branch is found");
    require(text->find("#define g_ScreenSpaceScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 320, 8)") !=
                std::string::npos, "the screen scale reads shared constant bytes 320");
    require(text->find("#define g_ScreenSpaceScale") < text->find("#else"), "only the SPIR-V branch gains it");
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
    require(text->find("return vk::RawBufferLoad<float4>(vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 328, 8)"
                       " + uint64_t(uint(index) & 1023u) * 16, 0x10);") != std::string::npos,
            "a palette read loads from the palette address");
    require(text->find("int4 i3 = vk::RawBufferLoad<int4>(vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 336, 8)"
                       " + uint64_t(3) * 16, 0x10);") != std::string::npos,
            "a loop constant loads from the loop address");
    require(text->find("my_g_LoopConstants[1]") != std::string::npos, "a longer name is left alone");
}
}

int main() {
    try {
        defines_the_screen_scale();
        reads_the_palette_and_loop_constants_through_addresses();
        std::cout << "Vulkan shader source checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
