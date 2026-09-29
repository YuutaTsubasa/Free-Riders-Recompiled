#pragma once
#include <stdexcept>
#include <string>

namespace sfr {
// Extend the pinned translator header without editing its upstream copy. The
// top descriptor bit marks framebuffer resolves only; assets retain their own
// dimensions, including float vertex textures. The ratio is passed at shader
// call sites, after the translator declares the shared constant buffer.
inline std::string resolution_shader_header(std::string text) {
    const auto replace = [&](const std::string& before, const std::string& after) {
        const auto at = text.find(before);
        if (at == std::string::npos) throw std::runtime_error("shader header lacks resolution anchor: " + before);
        text.replace(at, before.size(), after);
    };
    const std::string helper = R"(
float2 sfrTexture2DDimensions(uint descriptor, float2 logicalScale)
{
    float2 dimensions = getTexture2DDimensions(g_Texture2DDescriptorHeap[descriptor & 0x7fffffffu]);
    return (descriptor & 0x80000000u) ? round(dimensions * logicalScale) : dimensions;
}
float4 sfrTfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 logicalScale)
)";
    replace("float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)", helper);
    replace("float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)",
            "float2 sfrGetWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 logicalScale)");
    replace("float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)",
            "float4 sfrTfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 logicalScale)");
    replace("float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)",
            "float2 sfrGetPixelCoord(uint resourceDescriptorIndex, float2 texCoord, float2 logicalScale)");
    // Restrict rewrites to the four HLSL 2D helpers. Array/cube and the pinned
    // Metal implementation are unrelated to this runtime and stay untouched.
    for (const char* name : {"sfrTfetch2D(", "sfrGetWeights2D(", "sfrTfetch2DBicubic(", "sfrGetPixelCoord("}) {
        const auto start = text.find(name), end = text.find("\n}", start);
        if (start == std::string::npos || end == std::string::npos) throw std::runtime_error("malformed resolution shader helper");
        auto body = text.substr(start, end - start);
        const auto change = [&](const std::string& from, const std::string& to) {
            size_t at = 0;
            while ((at = body.find(from, at)) != std::string::npos) { body.replace(at, from.size(), to); at += to.size(); }
        };
        change("getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex])", "sfrTexture2DDimensions(resourceDescriptorIndex, logicalScale)");
        change("getTexture2DDimensions(texture)", "sfrTexture2DDimensions(resourceDescriptorIndex, logicalScale)");
        change("g_Texture2DDescriptorHeap[resourceDescriptorIndex]", "g_Texture2DDescriptorHeap[resourceDescriptorIndex & 0x7fffffffu]");
        text.replace(start, end - start, body);
    }
    text += R"(
#ifndef __air__
#define tfetch2D(index, sampler, coord, offset) sfrTfetch2D(index, sampler, coord, offset, g_ResolvedTextureScale)
#define getWeights2D(index, sampler, coord, offset) sfrGetWeights2D(index, sampler, coord, offset, g_ResolvedTextureScale)
#define tfetch2DBicubic(index, sampler, coord, offset) sfrTfetch2DBicubic(index, sampler, coord, offset, g_ResolvedTextureScale)
#define getPixelCoord(index, coord) sfrGetPixelCoord(index, coord, g_ResolvedTextureScale)
#endif
)";
    return text;
}
}
