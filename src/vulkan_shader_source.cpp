#include "vulkan_shader_source.h"
#include <charconv>
#include <cctype>
#include <string_view>

namespace sfr {
namespace {
// The shader constants as uniform buffers (set 2): vertex b0, pixel b1 and
// shared b2, each read as rows of four words at byte offsets, which is how
// the translator addresses them. Bound per draw with dynamic offsets into
// the upload ring (native_renderer.cpp).
//
// XenosRecomp's SPIR-V branch reads them through buffer addresses in push
// constants instead. A load through an address cannot move into the Adreno
// compiler's per-draw preamble (where uniform-buffer reads become constant
// registers), so every vertex of an AYN Thor race ran a median of 132
// memory reads, its vertex shaders reached half their occupancy, and vertex
// fetch stalled 40% of the time.
constexpr std::string_view uniform_buffers = R"hlsl(
[[vk::binding(0, 2)]] cbuffer SfrVertexShaderConstants { uint4 sfrRowsVS[256]; };
[[vk::binding(1, 2)]] cbuffer SfrPixelShaderConstants { uint4 sfrRowsPS[256]; };
[[vk::binding(2, 2)]] cbuffer SfrSharedConstants { uint4 sfrRowsSC[32]; };
#define SFR_UNIFORM_ACCESS(S) \
uint4 sfrRow##S(uint o) { return sfrRows##S[o >> 4]; } \
uint sfrWord##S(uint o) { return sfrRows##S[o >> 4][(o >> 2) & 3u]; } \
uint2 sfrWords2##S(uint o) { return uint2(sfrWord##S(o), sfrWord##S(o + 4u)); } \
uint3 sfrWords3##S(uint o) { return uint3(sfrWord##S(o), sfrWord##S(o + 4u), sfrWord##S(o + 8u)); } \
uint4 sfrWords4##S(uint o) { return uint4(sfrWord##S(o), sfrWord##S(o + 4u), sfrWord##S(o + 8u), sfrWord##S(o + 12u)); }
SFR_UNIFORM_ACCESS(VS)
SFR_UNIFORM_ACCESS(PS)
SFR_UNIFORM_ACCESS(SC)
)hlsl";

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

constexpr const char* address_fields[] = {"VertexShaderConstants", "PixelShaderConstants",
                                        "SharedConstants", "VertexPalette", "LoopConstants"};
struct UniformField { const char* field; const char* suffix; };
constexpr UniformField uniform_fields[] = {
    {"VertexShaderConstants", "VS"}, {"PixelShaderConstants", "PS"}, {"SharedConstants", "SC"}};

// These are the pinned translator's scalar/vector loads through push-constant
// addresses. Each becomes a read of the matching uniform buffer. Refuse a
// changed header instead of guessing at an unfamiliar address expression.
bool replace_physical_loads(std::string& text) {
    constexpr std::string_view prefix = "vk::RawBufferLoad<";
    size_t at = 0;
    while ((at = text.find(prefix, at)) != std::string::npos) {
        const size_t type_end = text.find('>', at + prefix.size());
        if (type_end == std::string::npos || text.substr(type_end, 2) != ">(") return false;
        const std::string type = text.substr(at + prefix.size(), type_end - at - prefix.size());
        std::string base;
        int components = 0;
        for (const char* candidate : {"bool", "uint", "int", "float"}) {
            const std::string name = candidate;
            if (type == name) { base = name; components = 1; break; }
            if (type.size() == name.size() + 1 && type.starts_with(name) && type.back() >= '2' && type.back() <= '4' &&
                name != "bool") {
                base = name;
                components = type.back() - '0';
                break;
            }
        }
        if (!components) return false;
        size_t depth = 1, end = type_end + 2, comma = std::string::npos;
        for (; end < text.size() && depth; ++end) {
            if (text[end] == '(') ++depth;
            else if (text[end] == ')') --depth;
            else if (text[end] == ',' && depth == 1) {
                if (comma != std::string::npos) return false;
                comma = end;
            }
        }
        if (depth) return false;
        const size_t expression_end = comma == std::string::npos ? end - 1 : comma;
        const std::string expression = trim(std::string_view(text).substr(type_end + 2, expression_end - type_end - 2));
        std::string suffix, offset;
        for (const auto& [field, field_suffix] : uniform_fields) {
            const std::string access = std::string("g_PushConstants.") + field;
            if (!expression.starts_with(access)) continue;
            const std::string rest = trim(std::string_view(expression).substr(access.size()));
            if (rest.empty() || rest[0] != '+') continue;
            suffix = field_suffix;
            offset = trim(std::string_view(rest).substr(1));
            break;
        }
        if (suffix.empty() || offset.empty() || offset.find("uint64_t") != std::string::npos) return false;
        std::string alignment = comma == std::string::npos ? "4" :
            trim(std::string_view(text).substr(comma + 1, end - comma - 2));
        std::string_view digits = alignment;
        const int radix = digits.starts_with("0x") ? 16 : 10;
        if (radix == 16) digits.remove_prefix(2);
        unsigned bytes = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), bytes, radix);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
            bytes == 0 || bytes > 16 || (bytes & (bytes - 1))) return false;
        const std::string where = "(uint(" + offset + "))";
        std::string words;
        if (components == 4 && bytes == 16) words = "sfrRow" + suffix + where;
        else if (components == 1) words = "sfrWord" + suffix + where;
        else words = "sfrWords" + std::to_string(components) + suffix + where;
        std::string replacement;
        if (base == "float") replacement = "asfloat(" + words + ")";
        else if (base == "int") replacement = "asint(" + words + ")";
        else if (base == "bool") replacement = "(" + words + " != 0u)";
        else replacement = words;
        replacement = "(" + replacement + ")";
        text.replace(at, end - at, replacement);
        at += replacement.size();
    }
    return true;
}

// Puts a [[vk::binding(binding, set)]] in front of the declaration that
// begins with this text; false if it is missing.
bool bind(std::string& text, std::string_view declaration, int binding, int set) {
    const size_t at = text.find(declaration);
    if (at == std::string::npos) return false;
    text.insert(at, "[[vk::binding(" + std::to_string(binding) + ", " + std::to_string(set) + ")]] ");
    return true;
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
    const std::string push = "[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;";
    if (text.find(push) == std::string::npos) return std::nullopt;
    // The extension of the shared constants this project adds (see
    // SharedConstants in native_renderer.h).
    text.insert(at + anchor.size(),
                "#define g_ScreenSpaceScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 320, 8)\n"
                "#define g_ResolvedTextureScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 328, 8)\n");

    // The palette and the loop constants stay the constant buffers the
    // translator declares (vertex_palette.cpp, loop_constants.cpp): b3 and
    // b4 of the same set.
    if (text.find("cbuffer VertexPalette : register(") != std::string::npos &&
        !bind(text, "cbuffer VertexPalette : register(", 3, 2)) return std::nullopt;
    if (text.find("cbuffer LoopConstants : register(") != std::string::npos &&
        !bind(text, "cbuffer LoopConstants : register(", 4, 2)) return std::nullopt;
    // The three texture heaps are one array of sampled images seen as three
    // types (set 0); the samplers are set 1. A pipeline layout has three sets,
    // under the four a Mali-G57 allows (Issue #1).
    for (const char* heap : {"Texture2D<float4> g_Texture2DDescriptorHeap[] : register(",
                             "Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[] : register(",
                             "TextureCube<float4> g_TextureCubeDescriptorHeap[] : register("})
        if (text.find(heap) != std::string::npos && !bind(text, heap, 0, 0)) return std::nullopt;
    if (const char* samplers = "SamplerState g_SamplerDescriptorHeap[] : register(";
        text.find(samplers) != std::string::npos && !bind(text, samplers, 0, 1)) return std::nullopt;

    // The push constants stay declared, unread; their addresses are 32-bit
    // pairs so that no shader asks for shaderInt64.
    if (const size_t push_at = text.find(push); push_at != std::string::npos) {
        text.insert(push_at + push.size(), uniform_buffers);
        for (const std::string field : address_fields) {
            const std::string declaration = "uint64_t " + field + ";";
            const size_t member = text.find(declaration);
            if (member == std::string::npos) return std::nullopt;
            text.replace(member, declaration.size(), "uint2 " + field + ";");
        }
    }
    if (!replace_physical_loads(text) || text.find("uint64_t") != std::string::npos) return std::nullopt;
    // The input mask is 32-bit. Preserve the old 64-bit shift's zero result
    // for indices 32..63, including its masked shift count for larger values.
    const std::string wide_mask = "(swappedFloats & (1ull << semanticIndex)) != 0";
    if (const auto mask = text.find(wide_mask); mask != std::string::npos)
        text.replace(mask, wide_mask.size(),
            "((semanticIndex & 63u) < 32u && (swappedFloats & (1u << (semanticIndex & 31u))) != 0)");
    return text;
}
}
