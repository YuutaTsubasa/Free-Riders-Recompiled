#include "texture_fetch.h"
#include <algorithm>

namespace sfr {
FetchWords merge_texture_fetch(const FetchWords& texture, const FetchWords& slot, uint8_t min_mip, uint8_t max_mip) {
    FetchWords out{};
    // rlwimi r25,r22,0,10,21: clamp and sign-RF fields stay with the slot.
    constexpr uint32_t sampler0 = 0x003FFC00;
    out[0] = (texture[0] & ~sampler0) | (slot[0] & sampler0);
    // Base address is translated; w1 bit 11 (rlwimi ...,0,20,20) stays with the slot.
    out[1] = (gpu_address(texture[1]) & ~0x800u) | (slot[1] & 0x800u);
    out[2] = texture[2];
    // rlwimi r23,r21,0,1,12: swizzle-adjacent filter fields w3[19:30] stay with the slot.
    constexpr uint32_t sampler3 = 0x7FF80000;
    out[3] = (texture[3] & ~sampler3) | (slot[3] & sampler3);
    // rlwimi r10,r4,0,30,21: only the mip range w4[2:9] comes from the texture,
    // then limited to [max(texture min, device min), min(texture max, device max)].
    constexpr uint32_t sampler4 = 0xFFFFFC03;
    uint32_t word4 = (texture[4] & ~sampler4) | (slot[4] & sampler4);
    const uint32_t lowest = std::max<uint32_t>((texture[4] >> 2) & 0xF, min_mip);
    const uint32_t highest = std::min<uint32_t>((texture[4] >> 6) & 0xF, max_mip);
    word4 = (word4 & ~0x3Cu) | ((lowest & 0xF) << 2);
    word4 = (word4 & ~0x3C0u) | ((highest & 0xF) << 6);
    out[4] = word4;
    // Mip address keeps w5[9:28] from the texture translated like the base;
    // w5[0:8] (packed mips, dimension low bits' neighbours) stay with the slot.
    const uint32_t mip = (texture[5] & 0x1FFFFE00) + ((((texture[5] >> 20) & 0xFFF) + 0x200) & 0x1000);
    out[5] = (mip & ~0x1FFu) | (slot[5] & 0x1FFu);
    return out;
}

TextureFetch decode_texture_fetch(const FetchWords& w) {
    TextureFetch f{};
    f.type = w[0] & 3;
    f.signs = (w[0] >> 2) & 0xFF;
    f.pitch = (w[0] >> 22) & 0x1FF;
    f.tiled = (w[0] >> 31) & 1;
    f.format = w[1] & 0x3F;
    f.endian = (w[1] >> 6) & 3;
    f.base_address = w[1] & 0xFFFFF000;
    f.numeric_integer = w[3] & 1;
    f.swizzle = (w[3] >> 1) & 0xFFF;
    const int32_t exp = (w[3] >> 13) & 0x3F;
    f.exp_adjust = exp >= 32 ? exp - 64 : exp;
    f.min_mip = (w[4] >> 2) & 0xF;
    f.max_mip = (w[4] >> 6) & 0xF;
    f.packed_mips = w[5] & 1;
    f.dimension = static_cast<TextureDimension>((w[5] >> 9) & 3);
    f.mip_address = w[5] & 0xFFFFF000;
    switch (f.dimension) {
    case TextureDimension::one:
        f.width = (w[2] & 0xFFFFFF) + 1; f.height = 1; f.depth = 1; break;
    case TextureDimension::two:
    case TextureDimension::cube:
        f.width = (w[2] & 0x1FFF) + 1; f.height = ((w[2] >> 13) & 0x1FFF) + 1;
        f.depth = ((w[2] >> 26) & 0x3F) + 1; break;
    case TextureDimension::three:
        f.width = (w[2] & 0x7FF) + 1; f.height = ((w[2] >> 11) & 0x7FF) + 1;
        f.depth = ((w[2] >> 22) & 0x3FF) + 1; break;
    }
    return f;
}
}
