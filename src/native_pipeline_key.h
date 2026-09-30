#pragma once
#include "native_renderer.h"
#include <cstring>

namespace sfr {
// Reference serializer retained for same-binary performance comparisons.
// Serialize named fields, never struct padding: padding is not pipeline state.
inline void native_pipeline_key_legacy(const NativeDraw& draw, std::vector<uint8_t>& key) {
    key.clear();
    const auto put = [&](const void* data, size_t size) {
        key.insert(key.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    };
    put(&draw.vertex_shader, sizeof(void*));
    put(&draw.pixel_shader, sizeof(void*));
    put(&draw.pixel_spec_constants, 4);
    for (const auto& e : draw.elements) {
        put(&e.semanticIndex, 4); put(&e.location, 4); put(&e.format, sizeof(e.format));
        put(&e.slotIndex, 4); put(&e.alignedByteOffset, 4);
        // The name by its address: every one comes from declaration_semantic's
        // static table, so the pointer identifies it without a strlen a draw.
        put(&e.semanticName, sizeof(e.semanticName));
    }
    put(&draw.stride, 4); put(&draw.topology, sizeof(draw.topology));
    put(&draw.blend.srcBlend, sizeof(draw.blend.srcBlend)); put(&draw.blend.dstBlend, sizeof(draw.blend.dstBlend));
    put(&draw.blend.blendOp, sizeof(draw.blend.blendOp));
    put(&draw.blend.srcBlendAlpha, sizeof(draw.blend.srcBlendAlpha));
    put(&draw.blend.dstBlendAlpha, sizeof(draw.blend.dstBlendAlpha));
    put(&draw.blend.blendOpAlpha, sizeof(draw.blend.blendOpAlpha)); put(&draw.blend.blendEnabled, 1);
    put(&draw.write_mask, 1); put(&draw.depth_enabled, 1); put(&draw.depth_write, 1);
    put(&draw.depth_function, sizeof(draw.depth_function)); put(&draw.cull, sizeof(draw.cull));
    put(&draw.stencil_enabled, 1);
    if (draw.stencil_enabled) {
        put(&draw.stencil_reference, 1); put(&draw.stencil_read_mask, 1); put(&draw.stencil_write_mask, 1);
        for (const auto* face : {&draw.stencil_front, &draw.stencil_back}) {
            put(&face->passOp, sizeof(face->passOp)); put(&face->failOp, sizeof(face->failOp));
            put(&face->depthFailOp, sizeof(face->depthFailOp)); put(&face->compareFunction, sizeof(face->compareFunction));
        }
    }
}

// Same bytes as the reference, but one resize instead of an insertion for
// every field. No cached guest state: every key reflects the current draw.
inline void native_pipeline_key_bulk(const NativeDraw& draw, std::vector<uint8_t>& key) {
    constexpr size_t element_bytes = 4 * sizeof(uint32_t) + sizeof(plume::RenderFormat) + sizeof(const char*);
    constexpr size_t blend_bytes = 4 * sizeof(plume::RenderBlend) + 2 * sizeof(plume::RenderBlendOperation) + sizeof(bool);
    constexpr size_t stencil_face_bytes = 3 * sizeof(plume::RenderStencilOp) + sizeof(plume::RenderComparisonFunction);
    const size_t fixed_bytes = sizeof(draw.vertex_shader) + sizeof(draw.pixel_shader) +
        sizeof(draw.pixel_spec_constants) + sizeof(draw.stride) + sizeof(draw.topology) + blend_bytes +
        sizeof(draw.write_mask) + sizeof(draw.depth_enabled) + sizeof(draw.depth_write) +
        sizeof(draw.depth_function) + sizeof(draw.cull) + sizeof(draw.stencil_enabled);
    const size_t stencil_bytes = draw.stencil_enabled ? sizeof(draw.stencil_reference) +
        sizeof(draw.stencil_read_mask) + sizeof(draw.stencil_write_mask) +
        2 * stencil_face_bytes : 0;
    key.resize(fixed_bytes + draw.elements.size() * element_bytes + stencil_bytes);
    uint8_t* next = key.data();
    const auto put = [&]<typename T>(const T& value) {
        std::memcpy(next, &value, sizeof(value));
        next += sizeof(value);
    };
    put(draw.vertex_shader); put(draw.pixel_shader); put(draw.pixel_spec_constants);
    for (const auto& e : draw.elements) {
        put(e.semanticIndex); put(e.location); put(e.format); put(e.slotIndex); put(e.alignedByteOffset); put(e.semanticName);
    }
    put(draw.stride); put(draw.topology);
    put(draw.blend.srcBlend); put(draw.blend.dstBlend); put(draw.blend.blendOp);
    put(draw.blend.srcBlendAlpha); put(draw.blend.dstBlendAlpha); put(draw.blend.blendOpAlpha); put(draw.blend.blendEnabled);
    put(draw.write_mask); put(draw.depth_enabled); put(draw.depth_write);
    put(draw.depth_function); put(draw.cull); put(draw.stencil_enabled);
    if (draw.stencil_enabled) {
        put(draw.stencil_reference); put(draw.stencil_read_mask); put(draw.stencil_write_mask);
        for (const auto* face : {&draw.stencil_front, &draw.stencil_back}) {
            put(face->passOp); put(face->failOp); put(face->depthFailOp); put(face->compareFunction);
        }
    }
}
}
