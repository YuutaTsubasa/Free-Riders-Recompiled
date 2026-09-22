#include "loop_constants.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

std::string shader(const std::string& body, const char* declared = "") {
    return std::string("struct VertexShaderInput\n{\n\tfloat4 iPosition0 : POSITION0;\n};\n"
                       "void shaderMain(VertexShaderInput input)\n{\n\tfloat4 r0 = 0.0;\n") +
           declared + "\tint a0 = 0;\n\tint aL = 0;\n" + body + "}\n";
}

void run() {
    const auto none = sfr::add_loop_constants(shader("\tr0 = input.iPosition0;\n"));
    require(!none, "a shader without loops is unchanged");

    const auto declared = sfr::add_loop_constants(
        shader("\tfor (aL = 0; aL < i2.x; aL++)\n\t{\n\t}\n", "\tint4 i2 = int4(4, 0, 1, 0);\n"));
    require(!declared, "a loop the container defines is unchanged");

    const auto text = sfr::add_loop_constants(shader("\tfor (aL = 0; aL < i0.x; aL++)\n\t{\n\t}\n"
                                                     "\t\t\tif (aL < i0.x)\n"));
    require(text.has_value(), "an undeclared loop constant is declared");
    require(text->find("cbuffer LoopConstants : register(b4, space4)") != std::string::npos,
            "the draw supplies them in b4");
    require(text->find("\tint aL = 0;\n\tint4 i0 = g_LoopConstants[0];\n") != std::string::npos,
            "the declaration precedes every use");
    require(text->find("i0 = g_LoopConstants[0]", text->find("i0 = g_LoopConstants[0]") + 1) == std::string::npos,
            "each constant is declared once");

    require(!sfr::add_loop_constants(shader("\tfor (aL = 0; aL < i20.x; aL++)\n\t{\n\t}\n")),
            "a register the device shadow does not hold is refused");
    require(!sfr::add_loop_constants("void main() { for (aL = 0; aL < i0.x; aL++) {} }"),
            "a pixel shader is refused");
    require(!sfr::add_loop_constants(shader("\tr0 = iPosition0 + oTexCoord1;\n")),
            "names that merely start with i are not registers");
}
}

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << "loop_constants_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "loop_constants_test: ok\n";
    return 0;
}
