#pragma once
#include "native_renderer.h"
#include <cstring>

namespace sfr {
// Reference serializer retained for same-binary performance comparisons.
inline void native_pipeline_key_legacy(const NativeDraw& draw, std::vector<uint8_t>& key) {
    key.clear();
    const auto put = [&](const void* data, size_t size) {
        key.insert(key.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    };
    put(&draw.vertex_shader, sizeof(void*));
    put(&draw.pixel_shader, sizeof(void*));
    put(&draw.pixel_spec_constants, 4);
    for (const auto& e : draw.elements) {
        put(&e.semanticIndex, 4); put(&e.format, sizeof(e.format)); put(&e.slotIndex, 4); put(&e.alignedByteOffset, 4);
        // The name by its address: every one comes from declaration_semantic's
        // static table, so the pointer identifies it without a strlen a draw.
        put(&e.semanticName, sizeof(e.semanticName));
    }
    put(&draw.stride, 4); put(&draw.topology, sizeof(draw.topology)); put(&draw.blend, sizeof(draw.blend));
    put(&draw.write_mask, 1); put(&draw.depth_enabled, 1); put(&draw.depth_write, 1);
    put(&draw.depth_function, sizeof(draw.depth_function)); put(&draw.cull, sizeof(draw.cull));
    put(&draw.stencil_enabled, 1);
    if (draw.stencil_enabled) {
        put(&draw.stencil_reference, 1); put(&draw.stencil_read_mask, 1); put(&draw.stencil_write_mask, 1);
        put(&draw.stencil_front, sizeof(draw.stencil_front)); put(&draw.stencil_back, sizeof(draw.stencil_back));
    }
}

// Same bytes as the reference, but one resize instead of an insertion for
// every field. No cached guest state: every key reflects the current draw.
inline void native_pipeline_key_bulk(const NativeDraw& draw, std::vector<uint8_t>& key) {
    constexpr size_t element_bytes = 3 * sizeof(uint32_t) + sizeof(plume::RenderFormat) + sizeof(const char*);
    const size_t fixed_bytes = sizeof(draw.vertex_shader) + sizeof(draw.pixel_shader) +
        sizeof(draw.pixel_spec_constants) + sizeof(draw.stride) + sizeof(draw.topology) + sizeof(draw.blend) +
        sizeof(draw.write_mask) + sizeof(draw.depth_enabled) + sizeof(draw.depth_write) +
        sizeof(draw.depth_function) + sizeof(draw.cull) + sizeof(draw.stencil_enabled);
    const size_t stencil_bytes = draw.stencil_enabled ? sizeof(draw.stencil_reference) +
        sizeof(draw.stencil_read_mask) + sizeof(draw.stencil_write_mask) +
        sizeof(draw.stencil_front) + sizeof(draw.stencil_back) : 0;
    key.resize(fixed_bytes + draw.elements.size() * element_bytes + stencil_bytes);
    uint8_t* next = key.data();
    const auto put = [&]<typename T>(const T& value) {
        std::memcpy(next, &value, sizeof(value));
        next += sizeof(value);
    };
    put(draw.vertex_shader); put(draw.pixel_shader); put(draw.pixel_spec_constants);
    for (const auto& e : draw.elements) {
        put(e.semanticIndex); put(e.format); put(e.slotIndex); put(e.alignedByteOffset); put(e.semanticName);
    }
    put(draw.stride); put(draw.topology); put(draw.blend);
    put(draw.write_mask); put(draw.depth_enabled); put(draw.depth_write);
    put(draw.depth_function); put(draw.cull); put(draw.stencil_enabled);
    if (draw.stencil_enabled) {
        put(draw.stencil_reference); put(draw.stencil_read_mask); put(draw.stencil_write_mask);
        put(draw.stencil_front); put(draw.stencil_back);
    }
}
}
