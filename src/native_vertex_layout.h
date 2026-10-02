#pragma once
#include "native_formats.h"
#include <array>
#include <stdexcept>

namespace sfr {
struct NativeVertexLayout {
    std::vector<plume::RenderInputElement> elements;
    std::vector<uint32_t> dec3n_offsets;
    std::array<uint32_t, 8> swapped{};
};
struct NativeVertexLayoutError : std::runtime_error {
    uint32_t value;
    NativeVertexLayoutError(uint32_t value, const char* message) : std::runtime_error(message), value(value) {}
};
// Each element is the title's original 12-byte, big-endian declaration.
NativeVertexLayout decode_native_vertex_layout(std::span<const uint8_t> bytes, uint32_t stride);
class NativeVertexLayoutCache {
public:
    // Caller validates and rereads the bytes on EVERY call, including hits.
    // A returned reference lasts only until a later call replaces its entry.
    const NativeVertexLayout& get(uint32_t identity, uint32_t stride, std::span<const uint8_t> bytes);
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
private:
    struct Entry {
        bool valid = false;
        uint32_t identity = 0, stride = 0;
        size_t size = 0;
        std::array<uint8_t, 192> bytes{};
        NativeVertexLayout layout;
    };
    std::array<Entry, 16> entries_;
    size_t last_ = 0, next_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
};
}
