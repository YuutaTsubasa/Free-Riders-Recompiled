#include "shader_inputs.h"
#include <iostream>
#include <algorithm>
#include <cmath>
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
    std::string legacy = *sfr::dedupe_vertex_inputs(repeated);
    legacy.replace(legacy.find("void after()"), std::string("void after()").size(),
                   "void shaderMain(VertexShaderInput input)");
    const auto raw = sfr::dec3n_vertex_shader_source(legacy);
    require(raw && count(*raw, "uint iPackedDec3n") == 9, "all nine raw semantic fields are declared once");
    require(count(*raw, "float4 iNormal2") == 2, "legacy declarations remain byte-for-byte");
    require(raw->find("g_VertexDec3n() & 4u") != std::string::npos &&
            raw->find("sfrDecodeDec3n(input.iPackedDec3n2)") != std::string::npos,
            "normal2 uses its explicit mask bit and scalar packed input");
    require(!sfr::dec3n_vertex_shader_source(*raw), "an already adapted source is rejected");
    require(!sfr::dec3n_vertex_shader_source("void shaderMain() {}"), "missing input structure is rejected");
    require(!sfr::dec3n_vertex_shader_source("struct VertexShaderInput\n{\n"), "truncated structure is rejected");
    require(!sfr::dec3n_vertex_shader_source(unique + unique), "multiple input structures are rejected");
    std::string position_only = unique;
    position_only.replace(position_only.find("void after()"), std::string("void after()").size(),
                          "void shaderMain(VertexShaderInput input)");
    const auto no_candidate = sfr::dec3n_vertex_shader_source(position_only);
    require(no_candidate && no_candidate->find("r0 = input.iPosition0") != std::string::npos,
            "canonical position-only VS still has a paired raw library");
    auto branched = legacy;
    branched.replace(branched.find("VertexShaderInput input"), std::string("VertexShaderInput input").size(),
                     "\n#ifdef __air__\n\tconstant PushConstants& g_PushConstants [[buffer(8)]],\n"
                     "\tVertexShaderInput input [[stage_in]]\n#else\n\tVertexShaderInput input\n#endif\n");
    require(sfr::dec3n_vertex_shader_source(branched).has_value(), "pinned two-branch vertex parameter is accepted");
    std::string crlf;
    for (char c : legacy) { if (c == '\n') crlf += '\r'; crlf += c; }
    require(sfr::dec3n_vertex_shader_source(crlf) == raw, "CRLF legacy HLSL normalizes to the same variant");
    auto bad = legacy;
    bad.replace(bad.find("float4 iNormal2"), 6, "uint4");
    require(!sfr::dec3n_vertex_shader_source(bad), "wrong referenced field type rejects the whole variant");
    bad = legacy;
    bad.replace(bad.rfind("float4 iNormal2"), 6, "int4");
    require(!sfr::dec3n_vertex_shader_source(bad), "wrong field type in another branch cannot hide");
    bad = legacy;
    bad.replace(bad.find("VertexShaderInput input"), std::string("VertexShaderInput input").size(),
                "VertexShaderInput other");
    require(!sfr::dec3n_vertex_shader_source(bad), "unknown input parameter is rejected");
    bad = legacy;
    bad.replace(bad.find("VertexShaderInput input"), std::string("VertexShaderInput input").size(),
                "VertexShaderInput input, VertexShaderInput extra");
    require(!sfr::dec3n_vertex_shader_source(bad), "multiple noncanonical input parameters are rejected");
    bad = legacy + "\nuint iPackedDec3nUnused;\n";
    require(!sfr::dec3n_vertex_shader_source(bad), "reserved packed member prefix collision is rejected");
    bad = legacy;
    bad.replace(bad.rfind("input.iNormal2"), std::string("input.iNormal2").size(), "input.iNormal0");
    require(!sfr::dec3n_vertex_shader_source(bad), "undeclared referenced candidate is rejected");
    std::string uses = "struct VertexShaderInput\n{\n";
    const char* names[] = {"Normal0", "Normal1", "Normal2", "Normal3", "Tangent0", "Tangent1",
                           "Tangent2", "Tangent3", "Binormal0"};
    const char* semantics[] = {"NORMAL0", "NORMAL1", "NORMAL2", "NORMAL3", "TANGENT0", "TANGENT1",
                               "TANGENT2", "TANGENT3", "BINORMAL0"};
    for (unsigned bit = 0; bit < 9; ++bit)
        uses += std::string("float4 i") + names[bit] + " : " + semantics[bit] + ";\n";
    uses += "};\nvoid shaderMain(VertexShaderInput input) {\n";
    for (const auto* name : names) uses += std::string("r0 = swapFloats(mask, (input.i") + name + "), 0);\n";
    uses += "r0 = input.iNormal00; r1 = input.iTexCoord0; r2 = notinput.iNormal0;\n}\n";
    const auto all = sfr::dec3n_vertex_shader_source(uses);
    require(all.has_value(), "all supported usage tokens adapt");
    for (unsigned bit = 0; bit < 9; ++bit)
        require(all->find("g_VertexDec3n() & " + std::to_string(1u << bit) + "u") != std::string::npos,
                "each usage has its own mask bit");
    require(count(*all, "swapFloats(mask,") == 9 && all->find("input.iNormal00") != std::string::npos &&
            all->find("input.iTexCoord0") != std::string::npos && all->find("notinput.iNormal0") != std::string::npos, "outer swaps and unrelated tokens remain");
    bad = legacy;
    bad.replace(bad.find(": NORMAL2;"), std::string(": NORMAL2;").size(), ": NORMAL1;");
    require(!sfr::dec3n_vertex_shader_source(bad), "candidate member cannot select another semantic index");
    bad = legacy;
    bad.replace(bad.find(": NORMAL2;"), std::string(": NORMAL2;").size(), ": TANGENT2;");
    require(!sfr::dec3n_vertex_shader_source(bad), "candidate member cannot select another usage");
    bad = repeated;
    bad.replace(bad.find("void after()"), std::string("void after()").size(),
                "void shaderMain(VertexShaderInput input)");
    require(!sfr::dec3n_vertex_shader_source(bad), "same-branch candidate duplicates remaining after dedupe refuse");
    const std::string literals = "// input.iNormal0\n/* input.iTangent0 */\n"
                                 "\"input.iNormal0 \\\" input.iTangent1\"; 'input.iBinormal0';\n";
    const auto lexical = sfr::dec3n_vertex_shader_source(uses + literals);
    require(lexical && lexical->find(literals) != std::string::npos &&
            count(*lexical, "g_VertexDec3n() & 1u") == 1,
            "comments and escaped quoted literals stay unchanged while real expressions adapt");
    const auto eof_comment = sfr::dec3n_vertex_shader_source(uses + "// input.iNormal0");
    require(eof_comment && eof_comment->ends_with("// input.iNormal0"), "a legal line comment may end at EOF");
    for (const std::string suffix : {"/* input.iNormal0", "\"input.iNormal0",
                                     "'input.iNormal0", "\"input.iNormal0\\"})
        require(!sfr::dec3n_vertex_shader_source(uses + suffix), "truncated comments or literals refuse");
    // Independent old CPU table arithmetic, not direct unquantized SNORM10.
    for (int bits = 0; bits < 1024; ++bits) {
        const int signed10 = bits >= 512 ? bits - 1024 : bits;
        const long expected = std::lround(std::max(float(signed10) / 511.0f, -1.0f) * 32767.0f);
        const int clamped = std::max(signed10, -511);
        const int magnitude = (std::abs(clamped) * 4681 + 36) / 73;
        require((clamped < 0 ? -magnitude : magnitude) == expected, "integer decoder matches all old table values");
    }

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
