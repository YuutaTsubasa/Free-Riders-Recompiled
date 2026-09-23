#include "native_formats.h"
#include <algorithm>
#include <cstring>
#include <utility>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace sfr {
using plume::RenderFormat;

const char* declaration_semantic(uint32_t usage) {
    switch (usage) {
    case 0: return "POSITION";
    case 1: return "BLENDWEIGHT";
    case 2: return "BLENDINDICES";
    case 3: return "NORMAL";
    case 4: return "PSIZE";
    case 5: return "TEXCOORD";
    case 6: return "TANGENT";
    case 7: return "BINORMAL";
    case 8: return "TESSFACTOR";
    case 9: return "POSITIONT";
    case 10: return "COLOR";
    case 11: return "FOG";
    case 12: return "DEPTH";
    case 13: return "SAMPLE";
    default: return nullptr;
    }
}

std::optional<DeclarationFormat> declaration_format(uint32_t type) {
    switch (type) {
    case 0x2C83A4: return DeclarationFormat{RenderFormat::R32_FLOAT, false};           // FLOAT1
    case 0x2C23A5: return DeclarationFormat{RenderFormat::R32G32_FLOAT, false};        // FLOAT2
    case 0x2A23B9: return DeclarationFormat{RenderFormat::R32G32B32_FLOAT, false};     // FLOAT3
    case 0x1A23A6: return DeclarationFormat{RenderFormat::R32G32B32A32_FLOAT, false};  // FLOAT4
    case 0x182886: return DeclarationFormat{RenderFormat::B8G8R8A8_UNORM, false};      // D3DCOLOR
    case 0x1A2286: case 0x1A2386: return DeclarationFormat{RenderFormat::R8G8B8A8_UINT, false};  // UBYTE4
    case 0x1A2086: case 0x1A2186: return DeclarationFormat{RenderFormat::R8G8B8A8_UNORM, false}; // UBYTE4N
    case 0x2C2359: return DeclarationFormat{RenderFormat::R16G16_SINT, true};           // SHORT2
    case 0x1A235A: return DeclarationFormat{RenderFormat::R16G16B16A16_SINT, true};     // SHORT4
    case 0x2C2159: return DeclarationFormat{RenderFormat::R16G16_SNORM, true};          // SHORT2N
    case 0x1A215A: return DeclarationFormat{RenderFormat::R16G16B16A16_SNORM, true};    // SHORT4N
    case 0x2C2059: return DeclarationFormat{RenderFormat::R16G16_UNORM, true};          // USHORT2N
    case 0x1A205A: return DeclarationFormat{RenderFormat::R16G16B16A16_UNORM, true};    // USHORT4N
    case 0x2C82A1: return DeclarationFormat{RenderFormat::R32_UINT, false};             // UINT1
    case 0x2A2190: case 0x2A2390: return DeclarationFormat{RenderFormat::R32_UINT, false};       // DEC3N_2/3
    case 0x2C235F: return DeclarationFormat{RenderFormat::R16G16_FLOAT, true};          // FLOAT16_2
    case 0x1A2360: return DeclarationFormat{RenderFormat::R16G16B16A16_FLOAT, true};    // FLOAT16_4
    default: return std::nullopt;
    }
}

// The size of one component of a vertex format, which Vulkan wants an
// attribute's offset aligned to.
uint32_t format_component_bytes(RenderFormat format) {
    switch (format) {
    case RenderFormat::R32_FLOAT: case RenderFormat::R32G32_FLOAT:
    case RenderFormat::R32G32B32_FLOAT: case RenderFormat::R32G32B32A32_FLOAT:
    case RenderFormat::R32_UINT:
        return 4;
    case RenderFormat::R16G16_SINT: case RenderFormat::R16G16B16A16_SINT:
    case RenderFormat::R16G16_SNORM: case RenderFormat::R16G16B16A16_SNORM:
    case RenderFormat::R16G16_UNORM: case RenderFormat::R16G16B16A16_UNORM:
    case RenderFormat::R16G16_FLOAT: case RenderFormat::R16G16B16A16_FLOAT:
        return 2;
    default:
        return 1;
    }
}

// Sixteen bytes a step with one shuffle: a race frame swaps twenty-odd
// megabytes of vertex data, and the byte-at-a-time loop was seven percent of
// its main thread (docs/performance.md). Every x86-64 host this runs on has
// SSSE3; this library is not built with it enabled, so the function asks.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("ssse3")))
#endif
void swap_words(std::span<uint8_t> bytes) {
    size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const __m128i order = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (; i + 16 <= bytes.size(); i += 16) {
        auto* const at = reinterpret_cast<__m128i*>(bytes.data() + i);
        _mm_storeu_si128(at, _mm_shuffle_epi8(_mm_loadu_si128(at), order));
    }
#endif
    for (; i + 4 <= bytes.size(); i += 4) {
        std::swap(bytes[i], bytes[i + 3]);
        std::swap(bytes[i + 1], bytes[i + 2]);
    }
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("ssse3")))
#endif
void swap_words_into(std::span<uint8_t> to, std::span<const uint8_t> from) {
    const size_t size = (std::min)(to.size(), from.size());
    size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const __m128i order = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (; i + 16 <= size; i += 16)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to.data() + i),
                         _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(from.data() + i)), order));
#endif
    for (; i + 4 <= size; i += 4) {
        to[i] = from[i + 3];
        to[i + 1] = from[i + 2];
        to[i + 2] = from[i + 1];
        to[i + 3] = from[i];
    }
    for (; i < size; ++i) to[i] = from[i];
}

// Eight or four indices a step: a race frame decodes three quarters of a
// million, and the one-at-a-time loop was about a millisecond of its main
// thread (docs/performance.md).
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("sse4.1")))
#endif
IndexScan decode_indices(const uint8_t* bytes, uint32_t count, bool wide, uint32_t base,
                         bool restart_enabled, uint32_t* out) {
    IndexScan scan;
    const uint32_t restart = wide ? 0xFFFFFFFFu : 0xFFFFu;
    uint32_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    __m128i lowest = _mm_set1_epi32(-1), highest = _mm_setzero_si128();
    const __m128i offset = _mm_set1_epi32(int32_t(base)), ones = _mm_set1_epi32(-1);
    if (!wide) {
        const __m128i order = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        for (; i + 8 <= count; i += 8) {
            const __m128i raw = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + 2 * i)), order);
            if (restart_enabled && _mm_movemask_epi8(_mm_cmpeq_epi16(raw, ones))) break;
            const __m128i low = _mm_add_epi32(_mm_cvtepu16_epi32(raw), offset);
            const __m128i high = _mm_add_epi32(_mm_cvtepu16_epi32(_mm_srli_si128(raw, 8)), offset);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), low);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i + 4), high);
            lowest = _mm_min_epu32(lowest, _mm_min_epu32(low, high));
            highest = _mm_max_epu32(highest, _mm_max_epu32(low, high));
        }
    } else {
        const __m128i order = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        for (; i + 4 <= count; i += 4) {
            const __m128i raw = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + 4 * i)), order);
            if (restart_enabled && _mm_movemask_epi8(_mm_cmpeq_epi32(raw, ones))) break;
            const __m128i value = _mm_add_epi32(raw, offset);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), value);
            lowest = _mm_min_epu32(lowest, value);
            highest = _mm_max_epu32(highest, value);
        }
    }
    alignas(16) uint32_t lanes[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes), lowest);
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes + 4), highest);
    for (int lane = 0; lane < 4; ++lane) {
        scan.lowest = (std::min)(scan.lowest, lanes[lane]);
        scan.highest = (std::max)(scan.highest, lanes[4 + lane]);
    }
#endif
    for (; i < count; ++i) {
        const uint8_t* at = bytes + size_t(i) * (wide ? 4 : 2);
        const uint32_t index = wide ? uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3]
                                    : uint32_t(at[0]) << 8 | at[1];
        if (restart_enabled && index == restart) {
            out[i] = restart;
            scan.restart = true;
            continue;
        }
        const uint32_t vertex = base + index;
        out[i] = vertex;
        scan.lowest = (std::min)(scan.lowest, vertex);
        scan.highest = (std::max)(scan.highest, vertex);
    }
    return scan;
}

void swap_texture_bytes(std::span<uint8_t> bytes, uint32_t endian) {
    switch (endian) {
    case 1:
        for (size_t i = 0; i + 2 <= bytes.size(); i += 2) std::swap(bytes[i], bytes[i + 1]);
        break;
    case 2:
        swap_words(bytes);
        break;
    case 3:
        for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
            std::swap(bytes[i], bytes[i + 2]);
            std::swap(bytes[i + 1], bytes[i + 3]);
        }
        break;
    default:
        break;
    }
}

uint32_t tiled_block_index(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t block_bytes) {
    const uint32_t aligned = (pitch_blocks + 31) & ~31u;
    const uint32_t log_bpp = (block_bytes >> 2) + ((block_bytes >> 1) >> (block_bytes >> 2));
    const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7);
    const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
    const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp;
}

std::vector<uint8_t> untile_texture(std::span<const uint8_t> tiled, const TextureLayout& layout) {
    const uint32_t pitch_blocks = layout.row_bytes / layout.block_bytes;
    std::vector<uint8_t> linear(size_t(layout.row_bytes) * layout.rows);
    for (uint32_t y = 0; y < layout.rows; ++y)
        for (uint32_t x = 0; x < pitch_blocks; ++x) {
            const size_t from = size_t(tiled_block_index(x, y, pitch_blocks, layout.block_bytes)) * layout.block_bytes;
            if (from + layout.block_bytes > tiled.size()) continue;
            std::memcpy(linear.data() + size_t(y) * layout.row_bytes + size_t(x) * layout.block_bytes,
                        tiled.data() + from, layout.block_bytes);
        }
    return linear;
}

bool depth_texture_placeholder = false;

std::optional<TextureLayout> linear_texture_layout(const TextureFetch& fetch) {
    if (fetch.dimension != TextureDimension::two) return std::nullopt;
    TextureLayout layout{};
    switch (fetch.format) {
    case 18: layout = {RenderFormat::BC1_UNORM, 4, 8}; break;   // k_DXT1
    case 19: layout = {RenderFormat::BC2_UNORM, 4, 16}; break;  // k_DXT2_3
    case 20: layout = {RenderFormat::BC3_UNORM, 4, 16}; break;  // k_DXT4_5
    case 2: layout = {RenderFormat::R8_UNORM, 1, 1}; break;     // k_8
    case 6:                                                       // k_8_8_8_8
        // After the 8-in-32 swap component X is the lowest byte. Identity
        // (XYZW) and D3DFMT_A8R8G8B8's ZYXW swizzles map to byte orders.
        if (fetch.endian != 2) return std::nullopt;
        if (fetch.swizzle == 0x688) layout = {RenderFormat::R8G8B8A8_UNORM, 1, 4};
        else if (fetch.swizzle == 0x60A) layout = {RenderFormat::B8G8R8A8_UNORM, 1, 4};
        else return std::nullopt;
        break;
    case 23:                                                      // k_24_8 (depth)
        if (!depth_texture_placeholder) return std::nullopt;
        layout = {RenderFormat::R8G8B8A8_UNORM, 1, 4};
        break;
    case 32:                                                      // k_16_16_16_16_FLOAT
        if (!depth_texture_placeholder) return std::nullopt;
        layout = {RenderFormat::R16G16B16A16_FLOAT, 1, 8};
        break;
    default: return std::nullopt;
    }
    layout.width = fetch.width;
    layout.height = fetch.height;
    // The fetch pitch counts 32-texel units; rows are rows of blocks.
    const uint32_t pitch_texels = (std::max)(fetch.pitch * 32, fetch.width);  // parenthesized: windows.h max
    layout.row_bytes = pitch_texels / layout.block_width * layout.block_bytes;
    layout.rows = (fetch.height + layout.block_width - 1) / layout.block_width;
    layout.tiled = fetch.tiled;
    layout.guest_rows = layout.rows;
    if (fetch.tiled) {
        // Tiled surfaces are stored in whole 32x32-block tiles. The tiling
        // below holds for blocks of 4 bytes or more (8888 and DXT); one-byte
        // texels tile differently and are not supported yet.
        if (layout.block_bytes < 4) return std::nullopt;
        layout.row_bytes = ((pitch_texels / layout.block_width + 31) & ~31u) * layout.block_bytes;
        layout.guest_rows = (layout.rows + 31) & ~31u;
    }
    return layout;
}

namespace {
// The colour half of a BC block (all of BC1): four RGBA texels from two
// RGB565 end points, written into out[16][4]. BC1's three-colour mode (first
// end point not above the second) makes index 3 transparent black; BC2 and
// BC3 always use four colours.
void decode_color_block(const uint8_t* block, bool bc1, uint8_t out[16][4]) {
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    uint8_t palette[4][4];
    const auto expand = [](uint16_t c, uint8_t* rgb) {
        rgb[0] = uint8_t((c >> 11 & 31) * 255 / 31);
        rgb[1] = uint8_t((c >> 5 & 63) * 255 / 63);
        rgb[2] = uint8_t((c & 31) * 255 / 31);
    };
    expand(c0, palette[0]);
    expand(c1, palette[1]);
    palette[0][3] = palette[1][3] = 255;
    const bool four = !bc1 || c0 > c1;
    for (int channel = 0; channel < 3; ++channel) {
        const int a = palette[0][channel], b = palette[1][channel];
        if (four) {
            palette[2][channel] = uint8_t((2 * a + b + 1) / 3);
            palette[3][channel] = uint8_t((a + 2 * b + 1) / 3);
        } else {
            palette[2][channel] = uint8_t((a + b) / 2);
            palette[3][channel] = 0;
        }
    }
    palette[2][3] = 255;
    palette[3][3] = four ? 255 : 0;
    const uint32_t indices = uint32_t(block[4]) | uint32_t(block[5]) << 8 | uint32_t(block[6]) << 16 | uint32_t(block[7]) << 24;
    for (int texel = 0; texel < 16; ++texel) std::memcpy(out[texel], palette[indices >> (2 * texel) & 3], 4);
}

// BC3's alpha half: two end points and sixteen 3-bit indices.
void decode_alpha_block(const uint8_t* block, uint8_t out[16][4]) {
    const int a0 = block[0], a1 = block[1];
    int palette[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (int i = 1; i < 5; ++i) palette[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t indices = 0;
    for (int i = 0; i < 6; ++i) indices |= uint64_t(block[2 + i]) << (8 * i);
    for (int texel = 0; texel < 16; ++texel) out[texel][3] = uint8_t(palette[indices >> (3 * texel) & 7]);
}
}

void decode_block_compression(std::vector<uint8_t>& bytes, TextureLayout& layout) {
    const RenderFormat format = layout.format;
    if (format != RenderFormat::BC1_UNORM && format != RenderFormat::BC2_UNORM && format != RenderFormat::BC3_UNORM) return;
    const uint32_t width = layout.width, height = layout.height;
    const uint32_t blocks_wide = (width + 3) / 4;
    std::vector<uint8_t> texels(size_t(width) * height * 4);
    uint8_t decoded[16][4];
    for (uint32_t by = 0; by < layout.rows; ++by)
        for (uint32_t bx = 0; bx < blocks_wide; ++bx) {
            const size_t at = size_t(by) * layout.row_bytes + size_t(bx) * layout.block_bytes;
            if (at + layout.block_bytes > bytes.size()) continue;
            const uint8_t* block = bytes.data() + at;
            if (format == RenderFormat::BC1_UNORM) {
                decode_color_block(block, true, decoded);
            } else {
                decode_color_block(block + 8, false, decoded);
                if (format == RenderFormat::BC2_UNORM) {
                    for (int texel = 0; texel < 16; ++texel)
                        decoded[texel][3] = uint8_t((block[texel / 2] >> (4 * (texel & 1)) & 15) * 17);
                } else {
                    decode_alpha_block(block, decoded);
                }
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
                    std::memcpy(texels.data() + (size_t(by * 4 + y) * width + bx * 4 + x) * 4, decoded[y * 4 + x], 4);
        }
    bytes = std::move(texels);
    layout.format = RenderFormat::R8G8B8A8_UNORM;
    layout.block_width = 1;
    layout.block_bytes = 4;
    layout.row_bytes = width * 4;
    layout.rows = height;
    layout.guest_rows = height;
    layout.tiled = false;
}

namespace {
plume::RenderTextureAddressMode address_mode(uint32_t clamp) {
    using Mode = plume::RenderTextureAddressMode;
    switch (clamp) {
    case 0: return Mode::WRAP;
    case 1: return Mode::MIRROR;
    case 2: case 4: return Mode::CLAMP;
    case 3: case 5: return Mode::MIRROR_ONCE;
    default: return Mode::BORDER;
    }
}
plume::RenderFilter filter(uint32_t value) {
    return value == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
}
}

plume::RenderSamplerDesc fetch_sampler(const FetchWords& words) {
    plume::RenderSamplerDesc desc;
    desc.addressU = address_mode((words[0] >> 10) & 7);
    desc.addressV = address_mode((words[0] >> 13) & 7);
    desc.addressW = address_mode((words[0] >> 16) & 7);
    desc.magFilter = filter((words[3] >> 19) & 3);
    desc.minFilter = filter((words[3] >> 21) & 3);
    desc.mipmapMode = ((words[3] >> 23) & 3) == 1 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
    desc.maxAnisotropy = 1;
    desc.anisotropyEnabled = false;
    return desc;
}
}
