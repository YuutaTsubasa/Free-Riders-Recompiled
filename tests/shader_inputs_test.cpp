#include "shader_inputs.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

const std::string unique =
    "void before() {}\n"
    "struct VertexShaderInput\n"
    "{\n"
    "#ifdef __air__\n"
    "\tfloat4 iPosition0 [[attribute(0)]];\n"
    "\tfloat4 iTexCoord0 [[attribute(13)]];\n"
    "#else\n"
    "\t[[vk::location(0)]] float4 iPosition0 : POSITION0;\n"
    "\t[[vk::location(13)]] float4 iTexCoord0 : TEXCOORD0;\n"
    "#endif\n"
    "};\n"
    "void after() { r0 = input.iPosition0; }\n";

const std::string repeated =
    "struct VertexShaderInput\n"
    "{\n"
    "#ifdef __air__\n"
    "\tfloat4 iPosition0 [[attribute(0)]];\n"
    "\tfloat4 iNormal2 [[attribute(6)]];\n"
    "\tfloat4 iNormal2 [[attribute(6)]];\n"
    "#else\n"
    "\t[[vk::location(0)]] float4 iPosition0 : POSITION0;\n"
    "\t[[vk::location(6)]] float4 iNormal2 : NORMAL2;\n"
    "\t[[vk::location(6)]] float4 iNormal2 : NORMAL2;\n"
    "#endif\n"
    "};\n"
    "void after() { r0 = input.iNormal2; }\n";

size_t count(const std::string& text, const std::string& what) {
    size_t total = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++total;
    return total;
}

void run() {
    require(!sfr::dedupe_vertex_inputs(unique), "a structure without repeats is unchanged");
    require(!sfr::dedupe_vertex_inputs("void main() {}"), "a pixel shader is unchanged");

    const auto text = sfr::dedupe_vertex_inputs(repeated);
    require(text.has_value(), "a repeated member is dropped");
    require(count(*text, "float4 iNormal2") == 2, "one declaration per branch is kept");
    require(count(*text, "iPosition0") == 2, "the member declared once per branch is kept");
    require(text->find("#else") != std::string::npos && text->find("#endif") != std::string::npos,
            "both branches are kept");
    require(text->find("void after() { r0 = input.iNormal2; }") != std::string::npos,
            "the body follows the structure");
}
}

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << "shader_inputs_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "shader_inputs_test: ok\n";
    return 0;
}
