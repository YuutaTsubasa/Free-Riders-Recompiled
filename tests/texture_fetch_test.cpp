#include "texture_fetch.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void run() {
    // Physical translation: low 29 bits, plus 4 KiB only for the 0xE0000000 view.
    static_assert(sfr::gpu_address(0xA0001000) == 0x00001000);
    static_assert(sfr::gpu_address(0x4006F886) == 0x0006F886);
    static_assert(sfr::gpu_address(0xE1234000) == 0x01235000);
    static_assert(sfr::gpu_address(0xDFFFF000) == 0x1FFFF000);

    // Expected words worked by hand from the original 0x824F4220 bit operations.
    const sfr::FetchWords texture{0xFFFFFFFF, 0x4006F886, 0x12345678, 0x00000000,
                                  0xFFFF024C, 0xE0100E01};
    const sfr::FetchWords slot{0x00000000, 0x00000000, 0xAAAAAAAA, 0xFFFFFFFF,
                               0x00000003, 0x0000013F};
    const auto merged = sfr::merge_texture_fetch(texture, slot, 5, 7);
    require(merged[0] == 0xFFC003FF, "w0 clamps and sign-RF come from the slot");
    require(merged[1] == 0x0006F086, "w1 base is translated and bit 11 comes from the slot");
    require(merged[2] == 0x12345678, "w2 size comes from the texture");
    require(merged[3] == 0x7FF80000, "w3 filters come from the slot");
    require(merged[4] == 0x000001D7, "w4 mip range is limited to [max(3,5), min(9,7)]");
    require(merged[5] == 0x00101F3F, "w5 mip address is translated, low 9 bits from the slot");

    // Device limits that do not bind keep the texture's own mip range.
    const auto open = sfr::merge_texture_fetch(texture, slot, 0, 15);
    require(((open[4] >> 2) & 0xF) == 3 && ((open[4] >> 6) & 0xF) == 9, "texture mip range within limits");

    // Decode a 64x32 tiled 2D texture.
    const sfr::FetchWords words{0x80000002u | (5u << 22), 0x0006F086, 63u | (31u << 13),
                                (0x2Au << 13) | (0x123u << 1) | 1u, (2u << 2) | (6u << 6), (1u << 9) | 1u | 0x00ABC000};
    const auto f = sfr::decode_texture_fetch(words);
    require(f.type == 2 && f.tiled && f.pitch == 5, "w0 type, tiling and pitch");
    require(f.format == 6 && f.endian == 2 && f.base_address == 0x0006F000, "w1 format, endian and base");
    require(f.dimension == sfr::TextureDimension::two && f.width == 64 && f.height == 32 && f.depth == 1,
            "2D size");
    require(f.numeric_integer && f.swizzle == 0x123 && f.exp_adjust == -22, "w3 number format, swizzle, exp adjust");
    require(f.min_mip == 2 && f.max_mip == 6, "w4 mip range");
    require(f.packed_mips && f.mip_address == 0x00ABC000, "w5 packed mips and mip address");

    const auto volume = sfr::decode_texture_fetch({2, 0, 7u | (3u << 11) | (1u << 22), 0, 0, 2u << 9});
    require(volume.dimension == sfr::TextureDimension::three && volume.width == 8 && volume.height == 4 &&
            volume.depth == 2, "3D size uses 11/11/10-bit fields");
}
}

int main() {
    try {
        run();
        std::cout << "texture fetch tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
