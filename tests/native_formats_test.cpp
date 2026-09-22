#include "native_formats.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        // Word swapping, over lengths that exercise the sixteen-byte steps,
        // the four-byte tail, and a trailing partial word left alone.
        for (const size_t length : {0u, 3u, 4u, 15u, 16u, 20u, 33u, 64u, 70u}) {
            std::vector<uint8_t> bytes(length);
            for (size_t i = 0; i < length; ++i) bytes[i] = uint8_t(i * 7 + 1);
            const auto original = bytes;
            std::vector<uint8_t> copied(length, 0xEE);
            sfr::swap_words_into(copied, original);
            sfr::swap_words(bytes);
            require(copied == bytes, "swap_words_into writes what swap_words does in place");
            for (size_t i = 0; i < length; ++i) {
                const size_t word = i / 4 * 4;
                const bool whole = word + 4 <= length;
                const uint8_t expected = whole ? original[word + 3 - (i - word)] : original[i];
                require(bytes[i] == expected, "swap_words reverses each whole word and leaves a partial one");
            }
        }
        // Index decoding matches a byte-at-a-time reference for both widths,
        // with and without restart indices, over lengths with vector tails.
        for (const bool wide : {false, true})
            for (const bool restart : {false, true})
                for (const uint32_t count : {0u, 1u, 7u, 8u, 9u, 17u, 40u}) {
                    const uint32_t size = wide ? 4 : 2;
                    std::vector<uint8_t> bytes(size_t(count) * size);
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t value = (i * 2654435761u) >> (wide ? 8 : 20);
                        if (restart && i % 11 == 5) value = wide ? 0xFFFFFFFFu : 0xFFFFu;
                        for (uint32_t b = 0; b < size; ++b) bytes[i * size + b] = uint8_t(value >> (8 * (size - 1 - b)));
                    }
                    std::vector<uint32_t> out(count);
                    const auto scan = sfr::decode_indices(bytes.data(), count, wide, 100, restart, out.data());
                    uint32_t lowest = ~0u, highest = 0;
                    bool cut = false;
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t index = 0;
                        for (uint32_t b = 0; b < size; ++b) index = index << 8 | bytes[i * size + b];
                        if (restart && index == (wide ? 0xFFFFFFFFu : 0xFFFFu)) {
                            require(out[i] == index, "a restart index is written as it is");
                            cut = true;
                            continue;
                        }
                        require(out[i] == index + 100, "an index is decoded big-endian plus the base");
                        lowest = (std::min)(lowest, index + 100);
                        highest = (std::max)(highest, index + 100);
                    }
                    require(scan.lowest == lowest && scan.highest == highest && scan.restart == cut,
                            "decode_indices reports the range and restarts of its indices");
                }
        // Tiling permutes the blocks of each 32x32-block tile within that tile.
        for (const uint32_t bytes : {4u, 8u, 16u}) {
            std::set<uint32_t> seen;
            for (uint32_t y = 0; y < 32; ++y)
                for (uint32_t x = 0; x < 64; ++x) seen.insert(sfr::tiled_block_index(x, y, 64, bytes));
            require(seen.size() == 64 * 32 && *seen.rbegin() == 64 * 32 - 1, "tiling is a permutation of two tiles");
        }
        require(sfr::tiled_block_index(0, 0, 32, 8) == 0, "first block stays first");

        // Untiling places each tiled block at its linear position.
        sfr::TextureLayout layout{plume::RenderFormat::R8G8B8A8_UNORM, 1, 4, 128, 8, 32, 8, true, 32};
        std::vector<uint8_t> tiled(32 * 32 * 4);
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 32; ++x) tiled[sfr::tiled_block_index(x, y, 32, 4) * 4] = uint8_t(x + y * 7);
        const auto linear = sfr::untile_texture(tiled, layout);
        require(linear.size() == 128 * 8, "untiled base level keeps its rows");
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 32; ++x) require(linear[y * 128 + x * 4] == uint8_t(x + y * 7), "block lands in place");

        // TEXCOORD4-7 take names XenosRecomp maps; others are unchanged.
        require(sfr::shader_input_usage(5, 3).usage == 5 && sfr::shader_input_usage(5, 3).index == 3, "TEXCOORD3 kept");
        require(sfr::shader_input_usage(5, 4).usage == 0 && sfr::shader_input_usage(5, 4).index == 1, "TEXCOORD4 is POSITION1");
        require(sfr::shader_input_usage(5, 6).usage == 0 && sfr::shader_input_usage(5, 6).index == 3, "TEXCOORD6 is POSITION3");
        require(sfr::shader_input_usage(5, 7).usage == 3 && sfr::shader_input_usage(5, 7).index == 1, "TEXCOORD7 is NORMAL1");

        // k_8_8_8_8 with 8-in-32 endianness: identity and A8R8G8B8 swizzles.
        sfr::TextureFetch fetch{};
        fetch.format = 6;
        fetch.endian = 2;
        fetch.width = fetch.height = 32;
        fetch.pitch = 1;
        fetch.dimension = sfr::TextureDimension::two;
        fetch.swizzle = 0x60A;
        require(sfr::linear_texture_layout(fetch)->format == plume::RenderFormat::B8G8R8A8_UNORM, "ZYXW is BGRA");
        fetch.swizzle = 0x688;
        require(sfr::linear_texture_layout(fetch)->format == plume::RenderFormat::R8G8B8A8_UNORM, "XYZW is RGBA");
        fetch.tiled = true;
        const auto tiled_layout = sfr::linear_texture_layout(fetch);
        require(tiled_layout && tiled_layout->tiled && tiled_layout->guest_rows == 32 && tiled_layout->row_bytes == 128,
                "tiled surfaces occupy whole tiles");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Native format checks passed\n";
}
