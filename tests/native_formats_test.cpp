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
        // Separate, unaligned buffers and unequal spans must preserve the
        // source and bytes outside the common prefix, including SIMD tails.
        for (size_t source_offset = 0; source_offset < 16; ++source_offset)
            for (size_t destination_offset = 0; destination_offset < 16; ++destination_offset)
                for (size_t length = 0; length <= 80; ++length) {
                    std::vector<uint8_t> source(length + 40), destination(length + 40, 0xD7);
                    for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i * 19 + 7);
                    const auto original = source;
                    const size_t destination_length = length / 2 + (length % 3);
                    const size_t common = (std::min)(length, destination_length);
                    sfr::swap_words_into(std::span(destination).subspan(destination_offset, destination_length),
                        std::span<const uint8_t>(source).subspan(source_offset, length));
                    require(source == original, "swap copy must leave its source unchanged");
                    for (size_t i = 0; i < destination.size(); ++i) {
                        uint8_t expected = 0xD7;
                        if (i >= destination_offset && i - destination_offset < common) {
                            const size_t local = i - destination_offset, word = local / 4 * 4;
                            expected = original[source_offset + (word + 4 <= common ? word + 3 - local % 4 : local)];
                        }
                        require(destination[i] == expected, "unaligned swap must respect both span bounds and partial words");
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

        // Exercise every lane that can end a SIMD batch, unsigned base
        // wraparound, unaligned input and guards around the output.
        for (const bool wide : {false, true})
            for (const bool restart : {false, true})
                for (uint32_t count : {0u, 1u, 3u, 4u, 7u, 8u, 9u, 15u, 16u, 17u, 33u, 65u, 257u})
                    for (uint32_t base : {0u, 100u, 0xffff0000u, 0xfffffffdu})
                        for (int cut_at = -2; cut_at < int(count); ++cut_at) {
                            const uint32_t size = wide ? 4 : 2;
                            const uint32_t sentinel = wide ? ~0u : 0xffffu;
                            std::vector<uint8_t> storage(size_t(count) * size + 3);
                            auto* bytes = storage.data() + 3;
                            std::vector<uint32_t> expected(count), output(count + 2, 0xaabbccddu);
                            sfr::IndexScan expected_scan;
                            for (uint32_t i = 0; i < count; ++i) {
                                uint32_t raw = (i * 2654435761u + 0xfffdu) & sentinel;
                                if (cut_at == -2 || int(i) == cut_at) raw = sentinel;
                                for (uint32_t b = 0; b < size; ++b)
                                    bytes[i * size + b] = uint8_t(raw >> (8 * (size - 1 - b)));
                                if (restart && raw == sentinel) {
                                    expected[i] = sentinel;
                                    expected_scan.restart = true;
                                } else {
                                    const uint32_t vertex = base + raw;
                                    expected[i] = vertex;
                                    expected_scan.lowest = (std::min)(expected_scan.lowest, vertex);
                                    expected_scan.highest = (std::max)(expected_scan.highest, vertex);
                                }
                            }
                            const auto actual = sfr::decode_indices(bytes, count, wide, base, restart, output.data() + 1);
                            require(std::equal(expected.begin(), expected.end(), output.begin() + 1),
                                    "SIMD/tail indices match scalar decoding including base wraparound");
                            require(actual.lowest == expected_scan.lowest && actual.highest == expected_scan.highest &&
                                    actual.restart == expected_scan.restart, "SIMD/tail range and restart match scalar");
                            require(output.front() == 0xaabbccddu && output.back() == 0xaabbccddu,
                                    "index decoder writes exactly count outputs");
                        }
        // Texture endian modes operate only on complete pairs/words, even
        // when the span is unaligned or ends beside unrelated data.
        for (uint32_t endian=0;endian<=4;++endian)
            for (size_t offset=0;offset<16;++offset)
                for (size_t length=0;length<=81;++length) {
                    std::vector<uint8_t> bytes(offset+length+17);
                    for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*29+11);
                    auto expected=bytes;
                    const size_t group=endian==1 ? 2 : 4;
                    const size_t mask=endian==1 ? 1 : endian==2 ? 3 : 2;
                    if(endian>=1 && endian<=3)
                        for(size_t i=0;i+group<=length;i+=group)
                            for(size_t b=0;b<group;++b)expected[offset+i+b]=bytes[offset+i+(b^mask)];
                    sfr::swap_texture_bytes(std::span(bytes).subspan(offset,length),endian);
                    require(bytes==expected,"texture swap preserves endian semantics, span guards and incomplete groups");
                }
        for(uint32_t block_bytes : {4u,8u,16u})
            for(uint32_t pitch : {32u,64u,96u})
                for(uint32_t rows : {1u,8u,31u,32u,33u,65u}) {
                    sfr::TextureLayout shape{plume::RenderFormat::R8G8B8A8_UNORM,1,block_bytes,
                        pitch*block_bytes,rows,pitch,rows,true,(rows+31)&~31u};
                    const size_t full=size_t(shape.row_bytes)*shape.guest_rows;
                    for(size_t size : {size_t(0),full/2+1,full-1,full}) {
                        std::vector<uint8_t> input(size+3);
                        for(size_t i=0;i<input.size();++i)input[i]=uint8_t(i*37+19);
                        const auto bytes=std::span<const uint8_t>(input).subspan(3,size);
                        std::vector<uint8_t> expected(size_t(shape.row_bytes)*rows);
                        for(uint32_t y=0;y<rows;++y)
                            for(uint32_t x=0;x<pitch;++x) {
                                const size_t from=size_t(sfr::tiled_block_index(x,y,pitch,block_bytes))*block_bytes;
                                if(from+block_bytes<=bytes.size())
                                    std::copy_n(bytes.data()+from,block_bytes,expected.data()+size_t(y)*shape.row_bytes+size_t(x)*block_bytes);
                            }
                        require(sfr::untile_texture(bytes,shape)==expected,"untile preserves every block and zero-fills missing input");
                    }
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

        // Xenia's packed base mip for 16x16 starts at (16,0) in its tile.
        // This reproduces the black road's white multiplier sampled from
        // the wrong, mostly empty quadrant of the captured small texture.
        fetch.width = fetch.height = 16;
        fetch.packed_mips = true;
        auto packed_base = *sfr::linear_texture_layout(fetch);
        std::vector<uint8_t> white_base(4096);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 16; x < 32; ++x)
                std::fill_n(white_base.data() + sfr::tiled_block_index(x,y,32,4)*4, 4, 255);
        const auto white_linear = sfr::untile_texture(white_base, packed_base);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16*4; ++x)
                require(white_linear[y*packed_base.row_bytes+x] == 255,
                        "packed 16x16 base reads the white region at tile x=16, not the black origin");
        // The offset follows rounded-up dimensions and is measured in blocks,
        // including BC textures. Both linear and tiled storage retain pitch.
        for (uint32_t format : {6u, 18u, 19u, 20u})
            for (bool tiled_storage : {false, true})
                for (const auto size : {std::pair{8u,64u}, std::pair{64u,8u}, std::pair{12u,9u},
                                        std::pair{16u,16u}, std::pair{64u,64u}}) {
                    auto packed_fetch = fetch;
                    packed_fetch.format = format;
                    packed_fetch.width = size.first; packed_fetch.height = size.second;
                    packed_fetch.pitch = 3; packed_fetch.tiled = tiled_storage;
                    auto shape = *sfr::linear_texture_layout(packed_fetch);
                    const bool packed = (std::min)(size.first,size.second) <= 16;
                    const uint32_t ox = packed && size.first != 64 ? 16 / shape.block_width : 0;
                    const uint32_t oy = packed && size.first == 64 ? 16 / shape.block_width : 0;
                    require(shape.base_x_blocks == ox && shape.base_y_blocks == oy,
                            "packed base offset uses log2 dimensions and block units");
                    require(shape.guest_rows >= shape.rows + oy,
                            "guest allocation includes rows before a vertically packed base");
                    if (!packed) continue;
                    const uint32_t pitch = shape.row_bytes / shape.block_bytes;
                    const uint32_t width = (shape.width + shape.block_width - 1) / shape.block_width;
                    std::vector<uint8_t> storage(size_t(shape.row_bytes) * shape.guest_rows, 0);
                    for (uint32_t y = 0; y < shape.rows; ++y)
                        for (uint32_t x = 0; x < width; ++x) {
                            const size_t block = tiled_storage ? sfr::tiled_block_index(x+ox,y+oy,pitch,shape.block_bytes)
                                                               : size_t(y+oy)*pitch+x+ox;
                            std::fill_n(storage.data()+block*shape.block_bytes,shape.block_bytes,uint8_t(1+x+y*7));
                        }
                    const auto extracted = sfr::untile_texture(storage,shape);
                    for (uint32_t y = 0; y < shape.rows; ++y)
                        for (uint32_t x = 0; x < width; ++x)
                            require(extracted[size_t(y)*shape.row_bytes+x*shape.block_bytes] == uint8_t(1+x+y*7),
                                    "packed base extraction applies its offset once without changing pitch");
                    require(sfr::untile_texture({},shape) == std::vector<uint8_t>(size_t(shape.row_bytes)*shape.rows),
                            "missing packed input stays zero without an out-of-bounds read");
                }

        // Issue 31's broad bright bands sample this linear k_4_4_4_4 fetch.
        // Falling back to white loses its mostly transparent alpha channel.
        auto band = sfr::decode_texture_fetch({0x02000002, 0x0f13404f, 0x0003e0ff,
                                               0x00280c14, 0x003b0003, 0x00000200});
        const auto band_layout = sfr::linear_texture_layout(band);
        require(band_layout.has_value(), "linear 4444 band texture must not use the white placeholder");
        require(band_layout->format == plume::RenderFormat::B8G8R8A8_UNORM &&
                band_layout->block_bytes == 2 && band_layout->row_bytes == 512 &&
                band_layout->rows == 32 && band_layout->guest_rows == 32,
                "4444 layout keeps the guest byte pitch before expansion");
        // Big-endian pairs become little-endian packed XYZW. Distinct nibbles
        // expose channel reversal; the second row exposes guest pitch padding.
        band.width = 2;
        band.height = 2;
        band.pitch = 1;
        auto packed_layout = *sfr::linear_texture_layout(band);
        std::vector<uint8_t> packed(128, 0xee);
        packed[0] = 0x01; packed[1] = 0x23;
        packed[2] = 0xf4; packed[3] = 0x56;
        packed[64] = 0x87; packed[65] = 0x89;
        packed[66] = 0x00; packed[67] = 0x00;
        sfr::swap_texture_bytes(packed, band.endian);
        sfr::decode_rgba4(packed, packed_layout);
        require(packed == std::vector<uint8_t>({0x33, 0x22, 0x11, 0x00, 0x66, 0x55, 0x44, 0xff,
                                               0x99, 0x88, 0x77, 0x88, 0x00, 0x00, 0x00, 0x00}),
                "4444 expansion preserves all four nibbles, transparent alpha and row padding");
        require(packed_layout.format == plume::RenderFormat::B8G8R8A8_UNORM &&
                packed_layout.block_bytes == 4 && packed_layout.row_bytes == 8 &&
                packed_layout.rows == 2 && packed_layout.guest_rows == 2 && !packed_layout.tiled,
                "expanded ZYXW texture uses BGRA8 with tight rows");
        // Captured format-15 texels 2646 and 2678: faint color and alpha must
        // survive, unlike the previous all-255 placeholder.
        auto sample_layout = *sfr::linear_texture_layout(band);
        sample_layout.height = 1;
        std::vector<uint8_t> sample{0x10, 0x01, 0x21, 0x11};
        sfr::swap_texture_bytes(sample, band.endian);
        sfr::decode_rgba4(sample, sample_layout);
        require(sample == std::vector<uint8_t>({17, 0, 0, 17, 17, 17, 17, 34}),
                "captured band texture samples retain their low alpha");
        band.swizzle = 0x688;
        band.endian = 0;
        auto identity_layout = *sfr::linear_texture_layout(band);
        require(identity_layout.format == plume::RenderFormat::R8G8B8A8_UNORM,
                "4444 identity swizzle uses RGBA8");
        std::vector<uint8_t> truncated{0x23, 0x81, 0xff};
        sfr::decode_rgba4(truncated, identity_layout);
        require(truncated.size() == 16 && truncated[0] == 0x33 && truncated[3] == 0x88 &&
                std::all_of(truncated.begin() + 4, truncated.end(), [](uint8_t b) { return b == 0; }),
                "4444 missing texels are transparent without reading past the input");
        for (int unsupported = 0; unsupported < 7; ++unsupported) {
            auto invalid = band;
            switch (unsupported) {
            case 0: invalid.tiled = true; break;
            case 1: invalid.endian = 2; break;
            case 2: invalid.swizzle = 0; break;
            case 3: invalid.signs = 1; break;
            case 4: invalid.numeric_integer = true; break;
            case 5: invalid.exp_adjust = 1; break;
            case 6: invalid.dimension = sfr::TextureDimension::three; break;
            }
            require(!sfr::linear_texture_layout(invalid), "unaudited 4444 fetch modes remain unsupported");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Native format checks passed\n";
}
