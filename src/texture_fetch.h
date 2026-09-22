#pragma once
#include <array>
#include <cstdint>

namespace sfr {
// A Xenos texture fetch constant: six words, as stored in a D3D texture header
// at +28 and in the device's fetch slots at +0x480 + 24 * slot.
using FetchWords = std::array<uint32_t, 6>;

// Xbox virtual address to GPU physical address, as the original SetTexture
// computes it: low 29 bits, plus 4 KiB for the 0xE0000000 physical view.
constexpr uint32_t gpu_address(uint32_t virtual_address) {
    return (virtual_address & 0x1FFFFFFF) + ((((virtual_address >> 20) & 0xFFF) + 0x200) & 0x1000);
}

// Original 0x824F4220 merge of a texture's fetch words into a device slot.
// The slot keeps its sampler state (clamps, filters, LOD); the texture supplies
// format, size and addresses; the mip range is limited by the per-slot device
// limits min_mip (+0x2F5E + slot) and max_mip (+0x2F78 + slot).
FetchWords merge_texture_fetch(const FetchWords& texture, const FetchWords& slot, uint8_t min_mip, uint8_t max_mip);

enum class TextureDimension : uint8_t { one = 0, two = 1, three = 2, cube = 3 };

// Decoded fields of a fetch constant (bit positions counted from the LSB).
struct TextureFetch {
    uint32_t type;           // w0[0:1], 2 = texture
    uint32_t signs;          // w0[2:9], per-component sign mode
    uint32_t pitch;          // w0[22:30], in 32-texel units
    bool tiled;              // w0[31]
    uint32_t format;         // w1[0:5]
    uint32_t endian;         // w1[6:7]
    uint32_t base_address;   // w1[12:31] << 12
    uint32_t width, height, depth;  // from w2 by dimension
    bool numeric_integer;    // w3[0]
    uint32_t swizzle;        // w3[1:12], three bits per component
    int32_t exp_adjust;      // w3[13:18], signed
    uint32_t min_mip, max_mip;      // w4[2:5], w4[6:9]
    bool packed_mips;        // w5[0]
    TextureDimension dimension;     // w5[9:10]
    uint32_t mip_address;    // w5[12:31] << 12
};
TextureFetch decode_texture_fetch(const FetchWords& words);
}
