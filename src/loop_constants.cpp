#include "loop_constants.h"
#include <cctype>
#include <set>

namespace sfr {
namespace {
// The identifier at `at`, when it is a loop constant register (i<N>).
std::optional<uint32_t> loop_register(const std::string& text, size_t at) {
    if (at && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_')) return std::nullopt;
    size_t digits = at + 1;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
    if (digits == at + 1 || digits - at > 3) return std::nullopt;
    if (digits < text.size() && (std::isalnum(static_cast<unsigned char>(text[digits])) || text[digits] == '_'))
        return std::nullopt;
    return uint32_t(std::stoul(text.substr(at + 1, digits - at - 1)));
}
}

std::optional<std::string> add_loop_constants(const std::string& hlsl) {
    std::set<uint32_t> used, defined;
    for (size_t at = hlsl.find('i'); at != std::string::npos; at = hlsl.find('i', at + 1)) {
        const auto index = loop_register(hlsl, at);
        if (!index) continue;
        // "int4 i3 = int4(...)" is the translator's own declaration.
        const bool declaration = at >= 5 && hlsl.compare(at - 5, 5, "int4 ") == 0;
        (declaration ? defined : used).insert(*index);
    }
    for (const uint32_t index : defined) used.erase(index);
    if (used.empty()) return std::nullopt;
    if (*used.rbegin() >= loop_constants) return std::nullopt;
    // Only the vertex registers reach the shader (see loop_constants.h).
    if (hlsl.find("struct VertexShaderInput") == std::string::npos) return std::nullopt;

    std::string text = hlsl;
    const std::string anchor = "\tint aL = 0;\n";
    const size_t at = text.find(anchor);
    if (at == std::string::npos) return std::nullopt;
    std::string declarations;
    for (const uint32_t index : used)
        declarations += "\tint4 i" + std::to_string(index) + " = g_LoopConstants[" + std::to_string(index) + "];\n";
    text.insert(at + anchor.size(), declarations);

    const std::string buffer = "cbuffer LoopConstants : register(b4, space4)\n{\n\tint4 g_LoopConstants[" +
                               std::to_string(loop_constants) + "];\n};\n\n";
    const size_t insert = text.find("struct VertexShaderInput");
    text.insert(insert, buffer);
    return text;
}
}
