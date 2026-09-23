#include "vulkan_shader_source.h"
#include <cctype>

namespace sfr {
namespace {
// Removes "cbuffer <name> : register(...)\n{ ... };\n" if present.
void remove_cbuffer(std::string& text, const std::string& name) {
    const size_t at = text.find("cbuffer " + name + " : register(");
    if (at == std::string::npos) return;
    const size_t end = text.find("};", at);
    if (end == std::string::npos) return;
    size_t cut = end + 2;
    if (cut < text.size() && text[cut] == '\n') ++cut;
    text.erase(at, cut - at);
}

// Replaces each "<array>[<index>]" (index up to the matching bracket) with
// the load that expression makes of the index.
template<class Load>
void replace_indexing(std::string& text, const std::string& array, Load load) {
    const std::string open = array + "[";
    size_t at = 0;
    while ((at = text.find(open, at)) != std::string::npos) {
        // A longer identifier ending in the same name is not this array.
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_')) {
            at += open.size();
            continue;
        }
        size_t depth = 1, end = at + open.size();
        for (; end < text.size() && depth; ++end) {
            if (text[end] == '[') ++depth;
            else if (text[end] == ']') --depth;
        }
        if (depth) return;
        const std::string index = text.substr(at + open.size(), end - 1 - (at + open.size()));
        const std::string replacement = load(index);
        text.replace(at, end - at, replacement);
        at += replacement.size();
    }
}
}

std::optional<std::string> vulkan_shader_source(const std::string& hlsl) {
    // The translator writes CRLF line ends on Windows.
    std::string text;
    text.reserve(hlsl.size());
    for (const char c : hlsl)
        if (c != '\r') text += c;
    const std::string anchor = "#define g_conditionalRenderingIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 316)\n";
    const size_t at = text.find(anchor);
    if (at == std::string::npos) return std::nullopt;
    // The alignments are explicit: vk::RawBufferLoad assumes four bytes, which
    // under-aligns a 64-bit load. A desktop driver ignores that; one that
    // honours it reads the palette and loop addresses wrongly, and with them
    // every skinned character and every loop-driven fetch.
    const std::string address = "vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + ";
    const std::string aligned = ", 8)";
    text.insert(at + anchor.size(),
                "#define g_ScreenSpaceScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 320, 8)\n");

    remove_cbuffer(text, "VertexPalette");
    replace_indexing(text, "g_VertexPalette", [&](const std::string& index) {
        return "vk::RawBufferLoad<float4>(" + address + std::to_string(vulkan_palette_address_offset) + aligned +
               " + uint64_t(" + index + ") * 16, 0x10)";
    });
    remove_cbuffer(text, "LoopConstants");
    replace_indexing(text, "g_LoopConstants", [&](const std::string& index) {
        return "vk::RawBufferLoad<int4>(" + address + std::to_string(vulkan_loop_address_offset) + aligned +
               " + uint64_t(" + index + ") * 16, 0x10)";
    });
    return text;
}
}
