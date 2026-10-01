#include "vulkan_shader_source.h"
#include <charconv>
#include <cctype>
#include <string_view>

namespace sfr {
namespace {
// PhysicalStorageBuffer64 supports a uint2-to-pointer bitcast without the
// optional Int64 arithmetic capability. Keep all five addresses as uint2.
constexpr std::string_view physical_loads = R"hlsl(
// SFR_ADDRESS_MATH_BEGIN
uint2 sfrOffsetBits(uint value, bool negative) { return uint2(value, negative ? 0xffffffffu : 0u); }
// Translator offsets are side-effect-free integer expressions. Test the sign
// before conversion, so an unsigned offset above INT_MAX stays positive.
#define sfrOffset(value) sfrOffsetBits(uint(value), (value) < 0)
uint2 sfrScale16(uint2 value) {
    return uint2(value.x << 4, (value.y << 4) | (value.x >> 28));
}
uint2 sfrAddAddress(uint2 address, uint2 offset) {
    uint low = address.x + offset.x;
    return uint2(low, address.y + offset.y + (low < address.x ? 1u : 0u));
}
// SFR_ADDRESS_MATH_END
template<typename T>
using SfrPhysicalPointer = vk::SpirvType<32, 8, 8, vk::Literal<vk::integral_constant<uint, 5349> >, T>;
template<typename T>
[[vk::ext_capability(5347)]]
[[vk::ext_extension("SPV_KHR_physical_storage_buffer")]]
[[vk::ext_instruction(124)]]
SfrPhysicalPointer<T> sfrPointerCast(uint2 address);
template<typename T>
[[vk::ext_instruction(61)]]
T sfrPointerLoad(SfrPhysicalPointer<T> pointer, [[vk::ext_literal]] uint operands, [[vk::ext_literal]] uint alignment);
template<typename T, uint Alignment>
T sfrRawBufferLoad(uint2 address, uint2 offset) {
    return sfrPointerLoad<T>(sfrPointerCast<T>(sfrAddAddress(address, offset)), 2, Alignment);
}
)hlsl";

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

constexpr const char* address_fields[] = {"VertexShaderConstants", "PixelShaderConstants",
                                        "SharedConstants", "VertexPalette", "LoopConstants"};

// These are the pinned translator's scalar/vector physical loads. Refuse a
// changed header instead of guessing at an unfamiliar address expression.
bool replace_physical_loads(std::string& text) {
    constexpr std::string_view prefix = "vk::RawBufferLoad<";
    size_t at = 0;
    while ((at = text.find(prefix, at)) != std::string::npos) {
        const size_t type_end = text.find('>', at + prefix.size());
        if (type_end == std::string::npos || text.substr(type_end, 2) != ">(") return false;
        const std::string type = text.substr(at + prefix.size(), type_end - at - prefix.size());
        if (type != "bool" && type != "uint" && type != "uint2" && type != "uint3" && type != "uint4" &&
            type != "int" && type != "int2" && type != "int3" && type != "int4" &&
            type != "float" && type != "float2" && type != "float3" && type != "float4") return false;
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
        std::string address, offset;
        for (const char* field : address_fields) {
            const std::string access = std::string("g_PushConstants.") + field;
            if (!expression.starts_with(access)) continue;
            const std::string rest = trim(std::string_view(expression).substr(access.size()));
            if (rest.empty() || rest[0] != '+') continue;
            address = access;
            offset = trim(std::string_view(rest).substr(1));
            break;
        }
        if (address.empty() || offset.empty() || offset.find("uint64_t") != std::string::npos) return false;
        std::string alignment = comma == std::string::npos ? "4" :
            trim(std::string_view(text).substr(comma + 1, end - comma - 2));
        std::string_view digits = alignment;
        const int radix = digits.starts_with("0x") ? 16 : 10;
        if (radix == 16) digits.remove_prefix(2);
        unsigned bytes = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), bytes, radix);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
            bytes == 0 || bytes > 16 || (bytes & (bytes - 1))) return false;
        std::string replacement = "(sfrRawBufferLoad<" + (type == "bool" ? std::string("uint") : type) +
            ", " + std::to_string(bytes) + ">(" + address + ", sfrOffset(" + offset + ")))";
        if (type == "bool") replacement = "(" + replacement + " != 0u)";
        text.replace(at, end - at, replacement);
        at += replacement.size();
    }
    return true;
}
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
bool replace_indexing(std::string& text, const std::string& array, Load load) {
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
        if (depth) return false;
        const std::string index = text.substr(at + open.size(), end - 1 - (at + open.size()));
        const std::string replacement = load(index);
        text.replace(at, end - at, replacement);
        at += replacement.size();
    }
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
    // The palette and the loop constants are addressed by push constants of
    // their own (runtime_shader_cache.cpp adds them to the pinned header).
    // Reading those addresses out of the shared constants instead cost a
    // 64-bit vk::RawBufferLoad in every draw that fetches either, and that
    // load was under-aligned: vk::RawBufferLoad assumes four bytes, which a
    // desktop driver forgives and a stricter one need not. The alignment of
    // the screen-space scale, which stays in the shared constants, is said.
    text.insert(at + anchor.size(),
                "#define g_ScreenSpaceScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 320, 8)\n"
                "#define g_ResolvedTextureScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 328, 8)\n");

    remove_cbuffer(text, "VertexPalette");
    if (!replace_indexing(text, "g_VertexPalette", [](const std::string& index) {
        return "sfrRawBufferLoad<float4, 16>(g_PushConstants.VertexPalette, sfrScale16(sfrOffset(" + index + ")))";
    })) return std::nullopt;
    remove_cbuffer(text, "LoopConstants");
    if (!replace_indexing(text, "g_LoopConstants", [](const std::string& index) {
        return "sfrRawBufferLoad<int4, 16>(g_PushConstants.LoopConstants, sfrScale16(sfrOffset(" + index + ")))";
    })) return std::nullopt;

    // Adreno 750's Qualcomm driver can misload 64-bit push constant members
    // in fragment shaders. Keep the 40-byte CPU ABI, but load each address
    // as two 32-bit words, including arithmetic on them. The same
    // representation in both stages also avoids stage-specific layouts.
    if (const size_t push_at = text.find(push); push_at != std::string::npos) {
        text.insert(push_at + push.size(), physical_loads);
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
