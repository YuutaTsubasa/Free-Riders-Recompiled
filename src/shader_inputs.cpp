#include "shader_inputs.h"
#include <set>

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
}
