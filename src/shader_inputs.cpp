#include "shader_inputs.h"
#include <set>
#include <algorithm>

namespace sfr {
namespace {
// The member a declaration line names, or nothing: the structure holds
// "float4 iTexCoord0 [[attribute(13)]];" and
// "[[vk::location(13)]] float4 iTexCoord0 : TEXCOORD0;", one per line.
std::optional<std::string> member(const std::string& line) {
    if (line.find("float") == std::string::npos && line.find("uint") == std::string::npos) return std::nullopt;
    const size_t at = line.find(" i");
    if (at == std::string::npos) return std::nullopt;
    const size_t end = line.find_first_of(" :;", at + 1);
    if (end == std::string::npos) return std::nullopt;
    return line.substr(at + 1, end - at - 1);
}
}

std::optional<std::string> dedupe_vertex_inputs(const std::string& hlsl) {
    const size_t start = hlsl.find("struct VertexShaderInput");
    if (start == std::string::npos) return std::nullopt;
    const size_t end = hlsl.find("\n};", start);
    if (end == std::string::npos) return std::nullopt;

    std::string result = hlsl.substr(0, start);
    std::set<std::string> declared;
    bool dropped = false;
    for (size_t at = start; at <= end;) {
        const size_t newline = hlsl.find('\n', at);
        const size_t stop = newline == std::string::npos ? hlsl.size() : newline + 1;
        const std::string line = hlsl.substr(at, stop - at);
        // Each preprocessor branch declares the same members once.
        if (line.find('#') != std::string::npos) declared.clear();
        const auto name = member(line);
        if (!name || declared.insert(*name).second) result += line;
        else dropped = true;
        at = stop;
    }
    if (!dropped) return std::nullopt;
    result += hlsl.substr(end + 1);
    return result;
}

std::optional<std::string> dec3n_vertex_shader_source(const std::string& legacyHlsl) {
    std::string hlsl;
    hlsl.reserve(legacyHlsl.size());
    for (size_t i = 0; i < legacyHlsl.size(); ++i)
        if (legacyHlsl[i] != '\r' || i + 1 == legacyHlsl.size() || legacyHlsl[i + 1] != '\n') hlsl += legacyHlsl[i];
    const std::string structure = "struct VertexShaderInput\n{\n";
    const size_t start = hlsl.find(structure);
    if (start == std::string::npos || hlsl.find(structure, start + structure.size()) != std::string::npos ||
        hlsl.find("SFR_DEC3N") != std::string::npos || hlsl.find("iPackedDec3n") != std::string::npos ||
        hlsl.find("sfrDecodeDec3n") != std::string::npos || hlsl.find("sfrDec3nComponent") != std::string::npos ||
        hlsl.find("g_VertexDec3n") != std::string::npos)
        return std::nullopt;
    const size_t end = hlsl.find("\n};", start);
    if (end == std::string::npos) return std::nullopt;
    const std::string original = hlsl.substr(end + 3);
    const size_t main = original.find(" shaderMain("), brace = original.find('{', main);
    if (main == std::string::npos || brace == std::string::npos ||
        original.find(" shaderMain(", main + 1) != std::string::npos) return std::nullopt;
    const auto identifier = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    const std::string parameter = "VertexShaderInput input";
    unsigned parameters = 0;
    for (size_t at = original.find("VertexShaderInput"); at != std::string::npos;
         at = original.find("VertexShaderInput", at + parameter.size())) {
        const size_t next = at + parameter.size();
        if (at < main || next > brace || original.compare(at, parameter.size(), parameter) != 0 ||
            (next < original.size() && identifier(original[next]))) return std::nullopt;
        ++parameters;
    }
    // The pinned translator emits the same parameter in the two Metal/HLSL branches.
    const std::string branches = "VertexShaderInput input [[stage_in]]\n#else\n\tVertexShaderInput input\n#endif";
    const size_t branch = original.find(branches, main);
    if (parameters != 1 && (parameters != 2 || branch == std::string::npos ||
        branch + branches.size() > brace)) return std::nullopt;
    const char* names[] = {"Normal0", "Normal1", "Normal2", "Normal3", "Tangent0",
                           "Tangent1", "Tangent2", "Tangent3", "Binormal0"};
    bool declared[9] = {}, air_declared[9] = {}, invalid[9] = {};
    bool air = false, conditional = false, otherwise = false;
    const auto trim = [](const std::string& line) {
        const size_t first = line.find_first_not_of(" \t");
        return first == std::string::npos ? std::string{} : line.substr(first, line.find_last_not_of(" \t") - first + 1);
    };
    for (size_t at = start; at < end;) {
        const size_t stop = std::min(hlsl.find('\n', at), end);
        const std::string line = hlsl.substr(at, stop - at), directive = trim(line);
        if (directive.starts_with('#')) {
            if (directive == "#ifdef __air__" && !conditional) { conditional = true; air = true; }
            else if (directive == "#else" && conditional && air) { air = false; otherwise = true; }
            else if (directive == "#endif" && conditional && otherwise) { conditional = false; otherwise = false; }
            else return std::nullopt;
        }
        for (unsigned bit = 0; bit < 9; ++bit) {
            const std::string name = std::string("i") + names[bit];
            const size_t field = line.find(" " + name);
            if (field != std::string::npos &&
                (field + 1 + name.size() == line.size() || !identifier(line[field + 1 + name.size()]))) {
                bool& seen = air ? air_declared[bit] : declared[bit];
                if (seen) return std::nullopt;
                seen = true;
                const size_t type = line.rfind("float4 ", field);
                invalid[bit] |= type == std::string::npos || type + 6 != field;
                if (!air) {
                    const std::string semantic = bit < 4 ? "NORMAL" + std::to_string(bit)
                        : bit < 8 ? "TANGENT" + std::to_string(bit - 4) : "BINORMAL0";
                    invalid[bit] |= trim(line.substr(field + 1 + name.size())) != ": " + semantic + ";";
                }
            }
        }
        at = stop + 1;
    }
    if (conditional) return std::nullopt;
    std::string rewritten;
    for (size_t at = 0; at < original.size();) {
        if (original.compare(at, 2, "//") == 0 || original.compare(at, 2, "/*") == 0) {
            const bool line = original[at + 1] == '/';
            const size_t end = original.find(line ? "\n" : "*/", at + 2);
            if (end == std::string::npos) {
                if (!line) return std::nullopt;
                rewritten += original.substr(at);
                break;
            }
            const size_t stop = end + (line ? 1 : 2);
            rewritten += original.substr(at, stop - at);
            at = stop;
            continue;
        }
        if (original[at] == '"' || original[at] == '\'') {
            const char quote = original[at];
            size_t end = at + 1;
            for (; end < original.size() && original[end] != quote; ++end) {
                if (original[end] == '\\') {
                    if (++end == original.size()) return std::nullopt;
                }
            }
            if (end == original.size()) return std::nullopt;
            rewritten += original.substr(at, end + 1 - at);
            at = end + 1;
            continue;
        }
        bool replaced = false;
        if (original.compare(at, 7, "input.i") == 0 &&
            (!at || (!identifier(original[at - 1]) && original[at - 1] != '.'))) {
            for (unsigned bit = 0; bit < 9; ++bit) {
                const std::string name = std::string("input.i") + names[bit];
                const size_t next = at + name.size();
                if (original.compare(at, name.size(), name) != 0 ||
                    (next < original.size() && identifier(original[next]))) continue;
                if (!declared[bit] || invalid[bit]) return std::nullopt;
                rewritten += "((g_VertexDec3n() & " + std::to_string(1u << bit) +
                    "u) != 0u ? sfrDecodeDec3n(input.iPackedDec3n" + std::to_string(bit) + ") : " + name + ")";
                at = next;
                replaced = true;
                break;
            }
        }
        if (!replaced) rewritten += original[at++];
    }
    std::string fields = "\n#ifndef __air__\n";
    for (unsigned bit = 0; bit < 9; ++bit)
        fields += "\tuint iPackedDec3n" + std::to_string(bit) + " : SFR_DEC3N" + std::to_string(bit) + ";\n";
    fields += "#endif";
    return hlsl.substr(0, end) + fields + hlsl.substr(end, 3) + R"(
uint g_VertexDec3n();
float sfrDec3nComponent(uint bits) {
    int s = max(int(bits << 22) >> 22, -511);
    uint q = (uint(abs(s)) * 4681u + 36u) / 73u;
    // Construct rounded SNORM16 bits; shader division can differ from the input assembler by one ULP.
    if (!q) return 0.0;
    uint high = firstbithigh(q);
    uint significand = q << (23u - high);
    significand += (significand + 16383u) / 32767u;
    uint value = ((112u + high) << 23) + significand - (1u << 23);
    return asfloat(value | (s < 0 ? 0x80000000u : 0u));
}
float4 sfrDecodeDec3n(uint packed) {
    return float4(sfrDec3nComponent(packed), sfrDec3nComponent(packed >> 10),
                  sfrDec3nComponent(packed >> 20), 1.0);
}
)" + rewritten;
}
}
