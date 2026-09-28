#include "image_decode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace sfr {
namespace {

void fail(std::string* error, const char* why) {
    if (error) *error = why;
}

// Bits out of a byte stream, least significant first, which is the order
// DEFLATE uses. JPEG reads the other way round and has its own reader below.
struct BitReader {
    const uint8_t* data = nullptr;
    size_t length = 0, at = 0;
    uint32_t bits = 0, held = 0;
    bool failed = false;

    uint32_t take(uint32_t needed) {
        while (held < needed) {
            if (at >= length) { failed = true; return 0; }
            bits |= uint32_t(data[at++]) << held;
            held += 8;
        }
        const uint32_t out = needed ? bits & ((1u << needed) - 1) : 0;
        bits >>= needed;
        held -= needed;
        return out;
    }
    void to_byte_boundary() { bits = 0; held = 0; }
};

// A canonical Huffman table, decoded a bit at a time: this is zlib's own way
// of doing it, and it needs nothing but the code lengths.
struct Huffman {
    uint16_t counts[16] = {};
    std::vector<uint16_t> symbols;

    void build(const uint8_t* lengths, size_t count) {
        for (uint16_t& entry : counts) entry = 0;
        for (size_t symbol = 0; symbol < count; ++symbol) ++counts[lengths[symbol]];
        counts[0] = 0;
        uint16_t offsets[16] = {};
        for (int length = 1; length < 16; ++length) offsets[length] = uint16_t(offsets[length - 1] + counts[length - 1]);
        symbols.assign(count, 0);
        for (size_t symbol = 0; symbol < count; ++symbol)
            if (lengths[symbol]) symbols[offsets[lengths[symbol]]++] = uint16_t(symbol);
    }
    int decode(BitReader& in) const {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length < 16; ++length) {
            code |= int(in.take(1));
            if (in.failed) return -1;
            const int count = counts[length];
            if (code - first < count) return symbols[size_t(index + code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_block(BitReader& in, const Huffman& lengths, const Huffman& distances,
                   std::vector<uint8_t>& out, size_t output_limit) {
    for (;;) {
        const int symbol = lengths.decode(in);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (out.size() >= output_limit) return false;
            out.push_back(uint8_t(symbol));
            continue;
        }
        if (symbol == 256) return true;
        const size_t which = size_t(symbol - 257);
        if (which >= 29) return false;
        const uint32_t length = length_base[which] + in.take(length_extra[which]);
        const int distance_symbol = distances.decode(in);
        if (distance_symbol < 0 || distance_symbol >= 30) return false;
        const uint32_t distance = distance_base[distance_symbol] + in.take(uint32_t(distance_extra[distance_symbol]));
        if (in.failed || !distance || distance > out.size()) return false;
        if (length > output_limit - out.size()) return false;
        const size_t from = out.size() - distance;
        for (uint32_t step = 0; step < length; ++step) out.push_back(out[from + step]);
    }
}

bool inflate_bounded(const uint8_t* bytes, size_t length, std::vector<uint8_t>& out,
                     bool zlib_header, size_t output_limit) {
    if (out.size() > output_limit) return false;
    if (zlib_header) {
        if (length < 2) return false;
        // The method has to be deflate, and a preset dictionary is not
        // something a PNG ever asks for.
        if ((bytes[0] & 0x0F) != 8 || (bytes[1] & 0x20)) return false;
        bytes += 2;
        length -= 2;
    }
    BitReader in{bytes, length};
    for (;;) {
        const uint32_t last = in.take(1);
        const uint32_t kind = in.take(2);
        if (in.failed) return false;
        if (kind == 0) {
            in.to_byte_boundary();
            if (in.at + 4 > in.length) return false;
            const uint32_t count = uint32_t(in.data[in.at]) | (uint32_t(in.data[in.at + 1]) << 8);
            in.at += 4;  // the count, then its complement
            if (in.at + count > in.length) return false;
            if (count > output_limit - out.size()) return false;
            out.insert(out.end(), in.data + in.at, in.data + in.at + count);
            in.at += count;
        } else if (kind == 1) {
            // The fixed tables, which RFC 1951 spells out.
            static const Huffman fixed_lengths = [] {
                uint8_t table[288];
                for (int symbol = 0; symbol < 288; ++symbol)
                    table[symbol] = symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8;
                Huffman out;
                out.build(table, 288);
                return out;
            }();
            static const Huffman fixed_distances = [] {
                uint8_t table[30];
                for (uint8_t& entry : table) entry = 5;
                Huffman out;
                out.build(table, 30);
                return out;
            }();
            if (!inflate_block(in, fixed_lengths, fixed_distances, out, output_limit)) return false;
        } else if (kind == 2) {
            const uint32_t length_count = in.take(5) + 257;
            const uint32_t distance_count = in.take(5) + 1;
            const uint32_t code_count = in.take(4) + 4;
            if (in.failed || length_count > 286 || distance_count > 30) return false;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t code_lengths[19] = {};
            for (uint32_t index = 0; index < code_count; ++index) code_lengths[order[index]] = uint8_t(in.take(3));
            if (in.failed) return false;
            Huffman codes;
            codes.build(code_lengths, 19);
            // The lengths for the two tables, run-length encoded by the third.
            std::vector<uint8_t> table(length_count + distance_count, 0);
            for (size_t index = 0; index < table.size();) {
                const int symbol = codes.decode(in);
                if (symbol < 0) return false;
                if (symbol < 16) { table[index++] = uint8_t(symbol); continue; }
                uint32_t repeat = 0;
                uint8_t value = 0;
                if (symbol == 16) {
                    if (!index) return false;
                    value = table[index - 1];
                    repeat = 3 + in.take(2);
                } else if (symbol == 17) {
                    repeat = 3 + in.take(3);
                } else {
                    repeat = 11 + in.take(7);
                }
                if (in.failed || index + repeat > table.size()) return false;
                while (repeat--) table[index++] = value;
            }
            Huffman lengths, distances;
            lengths.build(table.data(), length_count);
            distances.build(table.data() + length_count, distance_count);
            if (!inflate_block(in, lengths, distances, out, output_limit)) return false;
        } else {
            return false;
        }
        if (last) return true;
        if (in.failed) return false;
    }
}

}  // namespace

bool inflate(const uint8_t* bytes, size_t length, std::vector<uint8_t>& out, bool zlib_header) {
    return inflate_bounded(bytes, length, out, zlib_header, (std::numeric_limits<size_t>::max)());
}

namespace {

uint32_t big_endian(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
}

// One sample out of a row, whatever the bit depth is.
uint32_t sample_at(const uint8_t* row, uint32_t index, uint32_t depth) {
    switch (depth) {
    case 8: return row[index];
    case 16: return (uint32_t(row[index * 2]) << 8) | row[index * 2 + 1];
    default: {
        const uint32_t per_byte = 8 / depth;
        const uint8_t byte = row[index / per_byte];
        const uint32_t shift = 8 - depth * (index % per_byte + 1);
        return (byte >> shift) & ((1u << depth) - 1);
    }
    }
}

// A sample scaled to a byte, so that every depth comes out of here the same.
uint8_t to_byte(uint32_t value, uint32_t depth) {
    switch (depth) {
    case 8: return uint8_t(value);
    case 16: return uint8_t(value >> 8);
    case 4: return uint8_t(value * 17);
    case 2: return uint8_t(value * 85);
    case 1: return value ? 255 : 0;
    default: return uint8_t(value);
    }
}

}  // namespace

DecodedImage decode_png(const std::vector<uint8_t>& bytes, std::string* error) {
    DecodedImage out;
    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0) { fail(error, "not a PNG"); return {}; }

    uint32_t width = 0, height = 0, depth = 0, colour = 0, interlace = 0;
    std::vector<uint8_t> palette, palette_alpha, compressed;
    bool have_header = false;
    size_t at = 8;
    while (at + 8 <= bytes.size()) {
        const uint32_t length = big_endian(bytes.data() + at);
        const char* const type = reinterpret_cast<const char*>(bytes.data()) + at + 4;
        const size_t data_at = at + 8;
        if (data_at + length + 4 > bytes.size()) { fail(error, "a chunk reaches past the end of the file"); return {}; }
        if (!std::memcmp(type, "IHDR", 4) && length >= 13) {
            width = big_endian(bytes.data() + data_at);
            height = big_endian(bytes.data() + data_at + 4);
            depth = bytes[data_at + 8];
            colour = bytes[data_at + 9];
            interlace = bytes[data_at + 12];
            have_header = true;
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(bytes.begin() + int64_t(data_at), bytes.begin() + int64_t(data_at + length));
        } else if (!std::memcmp(type, "tRNS", 4)) {
            palette_alpha.assign(bytes.begin() + int64_t(data_at), bytes.begin() + int64_t(data_at + length));
        } else if (!std::memcmp(type, "IDAT", 4)) {
            compressed.insert(compressed.end(), bytes.begin() + int64_t(data_at), bytes.begin() + int64_t(data_at + length));
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        at = data_at + length + 4;
    }
    if (!have_header || !width || !height) { fail(error, "no usable header"); return {}; }
    if (interlace) { fail(error, "an interlaced PNG is not read"); return {}; }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) { fail(error, "an odd bit depth"); return {}; }
    if (colour != 0 && colour != 2 && colour != 3 && colour != 4 && colour != 6) { fail(error, "an odd colour type"); return {}; }
    if (colour == 3 && palette.empty()) { fail(error, "a paletted PNG with no palette"); return {}; }
    if (uint64_t(width) * height > 64u * 1024 * 1024) { fail(error, "larger than anything this will draw"); return {}; }

    const uint32_t channels = colour == 2 ? 3 : colour == 4 ? 2 : colour == 6 ? 4 : 1;
    const uint64_t row_bits = uint64_t(width) * channels * depth;
    const uint64_t row_bytes = (row_bits + 7) / 8;
    const uint32_t step = (std::max)(1u, channels * depth / 8);  // what a filter calls the pixel before

    std::vector<uint8_t> raw;
    const size_t expected_bytes = size_t((row_bytes + 1) * height);
    raw.reserve(expected_bytes);
    if (!inflate_bounded(compressed.data(), compressed.size(), raw, true, expected_bytes)) {
        fail(error, "the pixels do not decompress within the header's size");
        return {};
    }
    if (raw.size() < (row_bytes + 1) * height) { fail(error, "fewer pixels than the header promises"); return {}; }

    // Undo the per-row filters in place, each row against the one above it.
    std::vector<uint8_t> pixels(size_t(row_bytes) * height, 0);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t filter = raw[size_t(row) * (row_bytes + 1)];
        const uint8_t* const source = raw.data() + size_t(row) * (row_bytes + 1) + 1;
        uint8_t* const target = pixels.data() + size_t(row) * row_bytes;
        const uint8_t* const above = row ? target - row_bytes : nullptr;
        for (uint64_t index = 0; index < row_bytes; ++index) {
            const uint8_t left = index >= step ? target[index - step] : 0;
            const uint8_t up = above ? above[index] : 0;
            const uint8_t corner = above && index >= step ? above[index - step] : 0;
            uint32_t value = source[index];
            switch (filter) {
            case 0: break;
            case 1: value += left; break;
            case 2: value += up; break;
            case 3: value += (left + up) / 2; break;
            case 4: {
                // Paeth: whichever of the three neighbours the gradient is
                // nearest to.
                const int estimate = int(left) + int(up) - int(corner);
                const int to_left = std::abs(estimate - int(left));
                const int to_up = std::abs(estimate - int(up));
                const int to_corner = std::abs(estimate - int(corner));
                value += to_left <= to_up && to_left <= to_corner ? left : to_up <= to_corner ? up : corner;
                break;
            }
            default: fail(error, "an unknown row filter"); return {};
            }
            target[index] = uint8_t(value);
        }
    }

    out.width = width;
    out.height = height;
    out.rgba.assign(size_t(width) * height * 4, 255);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* const line = pixels.data() + size_t(row) * row_bytes;
        for (uint32_t column = 0; column < width; ++column) {
            uint8_t* const pixel = out.rgba.data() + (size_t(row) * width + column) * 4;
            const uint32_t first = column * channels;
            if (colour == 3) {
                const uint32_t index = sample_at(line, first, depth);
                if (index * 3 + 2 < palette.size()) {
                    pixel[0] = palette[index * 3];
                    pixel[1] = palette[index * 3 + 1];
                    pixel[2] = palette[index * 3 + 2];
                }
                pixel[3] = index < palette_alpha.size() ? palette_alpha[index] : 255;
            } else if (colour == 0 || colour == 4) {
                const uint8_t grey = to_byte(sample_at(line, first, depth), depth);
                pixel[0] = pixel[1] = pixel[2] = grey;
                if (colour == 4) pixel[3] = to_byte(sample_at(line, first + 1, depth), depth);
            } else {
                pixel[0] = to_byte(sample_at(line, first, depth), depth);
                pixel[1] = to_byte(sample_at(line, first + 1, depth), depth);
                pixel[2] = to_byte(sample_at(line, first + 2, depth), depth);
                if (colour == 6) pixel[3] = to_byte(sample_at(line, first + 3, depth), depth);
            }
        }
    }
    return out;
}

namespace {

// JPEG reads its bits the other way up, and a 0xFF byte inside the entropy
// coded data is followed by a 0x00 that is not part of it.
struct JpegBits {
    const uint8_t* data = nullptr;
    size_t length = 0, at = 0;
    uint32_t bits = 0, held = 0;
    bool failed = false;

    uint32_t take(uint32_t needed) {
        while (held < needed) {
            uint8_t byte = 0;
            if (at >= length) { failed = true; return 0; }
            if (data[at] == 0xFF) {
                // 0xFF inside the data is written as 0xFF 0x00; 0xFF anything
                // else is a marker, and is left where it is -- a restart has
                // to find it, and reading past it would be reading the next
                // part of the file as pixels.
                const uint8_t next = at + 1 < length ? data[at + 1] : 0xD9;
                if (next == 0x00) { byte = 0xFF; at += 2; }
                else byte = 0;
            } else {
                byte = data[at++];
            }
            bits = (bits << 8) | byte;
            held += 8;
        }
        const uint32_t out = needed ? (bits >> (held - needed)) & ((1u << needed) - 1) : 0;
        held -= needed;
        return out;
    }
    void restart() { bits = 0; held = 0; }
};

struct JpegHuffman {
    uint8_t lengths[17] = {};
    uint8_t values[256] = {};
    int minimum[17] = {}, maximum[17] = {}, first[17] = {};

    void build() {
        int code = 0, index = 0;
        for (int length = 1; length <= 16; ++length) {
            first[length] = index;
            minimum[length] = code;
            code += lengths[length];
            maximum[length] = lengths[length] ? code - 1 : -1;
            index += lengths[length];
            code <<= 1;
        }
    }
    int decode(JpegBits& in) const {
        int code = 0;
        for (int length = 1; length <= 16; ++length) {
            code = (code << 1) | int(in.take(1));
            if (in.failed) return -1;
            if (maximum[length] >= 0 && code <= maximum[length] && code >= minimum[length])
                return values[first[length] + code - minimum[length]];
        }
        return -1;
    }
};

// The value a Huffman symbol says how many bits of: JPEG stores these so that
// the top half of the range is positive and the bottom half negative.
int extend(uint32_t value, uint32_t bits) {
    if (!bits) return 0;
    return int(value) < (1 << (bits - 1)) ? int(value) - (1 << bits) + 1 : int(value);
}

const uint8_t zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// A plain separable inverse DCT. Slower than the clever ones and easier to be
// sure of; a model's textures are decoded once, at load.
void inverse_dct(const int* in, uint8_t* out, size_t stride) {
    struct Basis {
        float table[8][8];
        Basis() {
            for (int x = 0; x < 8; ++x)
                for (int u = 0; u < 8; ++u)
                    table[x][u] = float((u ? 0.5 : 0.5 / std::sqrt(2.0))
                                        * std::cos((2 * x + 1) * u * 3.14159265358979 / 16));
        }
    };
    static const Basis basis;
    float rows[64];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            float sum = 0;
            for (int u = 0; u < 8; ++u) sum += basis.table[x][u] * float(in[y * 8 + u]);
            rows[y * 8 + x] = sum;
        }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            float sum = 0;
            for (int v = 0; v < 8; ++v) sum += basis.table[y][v] * rows[v * 8 + x];
            const int value = int(std::lround(sum)) + 128;
            out[size_t(y) * stride + size_t(x)] = uint8_t(value < 0 ? 0 : value > 255 ? 255 : value);
        }
}

struct JpegComponent {
    uint32_t id = 0, across = 1, down = 1, quantiser = 0;
    uint32_t dc_table = 0, ac_table = 0;
    int previous = 0;
    uint32_t width = 0, height = 0, stride = 0;
    std::vector<uint8_t> pixels;
};

}  // namespace

DecodedImage decode_jpeg(const std::vector<uint8_t>& bytes, std::string* error) {
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) { fail(error, "not a JPEG"); return {}; }

    uint16_t quantisers[4][64] = {};
    JpegHuffman dc_tables[4], ac_tables[4];
    std::vector<JpegComponent> components;
    uint32_t width = 0, height = 0, restart_interval = 0;
    size_t at = 2;
    while (at + 4 <= bytes.size()) {
        if (bytes[at] != 0xFF) { ++at; continue; }
        const uint8_t marker = bytes[at + 1];
        at += 2;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (marker == 0xD9) break;
        if (at + 2 > bytes.size()) { fail(error, "a marker with no length"); return {}; }
        const size_t length = (size_t(bytes[at]) << 8) | bytes[at + 1];
        const size_t payload = at + 2;
        if (at + length > bytes.size() || length < 2) { fail(error, "a segment reaches past the end"); return {}; }
        const size_t segment_end = at + length;
        if (marker == 0xC0 || marker == 0xC1) {
            if (length < 8) { fail(error, "a short frame segment"); return {}; }
            height = (uint32_t(bytes[payload + 1]) << 8) | bytes[payload + 2];
            width = (uint32_t(bytes[payload + 3]) << 8) | bytes[payload + 4];
            const uint32_t count = bytes[payload + 5];
            if (count != 1 && count != 3) { fail(error, "only grey and colour JPEGs are read"); return {}; }
            if (length < 8 + size_t(count) * 3) { fail(error, "a short frame component list"); return {}; }
            components.clear();
            for (uint32_t index = 0; index < count; ++index) {
                const uint8_t* const entry = bytes.data() + payload + 6 + index * 3;
                JpegComponent component;
                component.id = entry[0];
                component.across = entry[1] >> 4;
                component.down = entry[1] & 15;
                component.quantiser = entry[2] & 3;
                if (!component.across || !component.down || component.across > 4 || component.down > 4) {
                    fail(error, "an odd sampling factor");
                    return {};
                }
                components.push_back(component);
            }
        } else if (marker == 0xC2) {
            fail(error, "a progressive JPEG is not read");
            return {};
        } else if (marker == 0xC4) {
            size_t walk = payload;
            while (walk < at + length) {
                if (segment_end - walk < 17) { fail(error, "a short Huffman table header"); return {}; }
                const uint8_t which = bytes[walk] & 3;
                const bool alternating = (bytes[walk] >> 4) != 0;
                JpegHuffman& table = alternating ? ac_tables[which] : dc_tables[which];
                ++walk;
                uint32_t total = 0;
                for (int bits = 1; bits <= 16; ++bits) {
                    table.lengths[bits] = bytes[walk + size_t(bits) - 1];
                    total += table.lengths[bits];
                }
                walk += 16;
                if (total > 256 || total > segment_end - walk) { fail(error, "a Huffman table that does not fit"); return {}; }
                for (uint32_t index = 0; index < total; ++index) table.values[index] = bytes[walk + index];
                walk += total;
                table.build();
            }
        } else if (marker == 0xDB) {
            size_t walk = payload;
            while (walk < at + length) {
                const uint8_t which = bytes[walk] & 3;
                const bool sixteen_bit = (bytes[walk] >> 4) != 0;
                ++walk;
                if (segment_end - walk < (sixteen_bit ? 128u : 64u)) {
                    fail(error, "a short quantisation table");
                    return {};
                }
                for (int index = 0; index < 64; ++index) {
                    quantisers[which][index] = sixteen_bit
                        ? uint16_t((uint32_t(bytes[walk]) << 8) | bytes[walk + 1])
                        : uint16_t(bytes[walk]);
                    walk += sixteen_bit ? 2 : 1;
                }
            }
        } else if (marker == 0xDD) {
            if (length < 4) { fail(error, "a short restart segment"); return {}; }
            restart_interval = (uint32_t(bytes[payload]) << 8) | bytes[payload + 1];
        } else if (marker == 0xDA) {
            if (length < 3) { fail(error, "a short scan segment"); return {}; }
            const uint32_t count = bytes[payload];
            if (length < 6 + size_t(count) * 2) { fail(error, "a short scan component list"); return {}; }
            for (uint32_t index = 0; index < count; ++index) {
                const uint8_t id = bytes[payload + 1 + index * 2];
                const uint8_t tables = bytes[payload + 2 + index * 2];
                for (JpegComponent& component : components)
                    if (component.id == id) {
                        component.dc_table = uint32_t(tables >> 4) & 3;
                        component.ac_table = uint32_t(tables) & 3;
                    }
            }
            at += length;
            break;  // what follows is the entropy coded data
        } else {
            at += length;
            continue;
        }
        at += length;
    }
    if (!width || !height || components.empty()) { fail(error, "no image in the file"); return {}; }
    if (uint64_t(width) * height > 64u * 1024 * 1024) { fail(error, "larger than anything this will draw"); return {}; }

    uint32_t widest = 1, tallest = 1;
    for (const JpegComponent& component : components) {
        widest = (std::max)(widest, component.across);
        tallest = (std::max)(tallest, component.down);
    }
    const uint32_t across_blocks = (width + widest * 8 - 1) / (widest * 8);
    const uint32_t down_blocks = (height + tallest * 8 - 1) / (tallest * 8);
    for (JpegComponent& component : components) {
        component.stride = across_blocks * component.across * 8;
        component.width = component.stride;
        component.height = down_blocks * component.down * 8;
        component.pixels.assign(size_t(component.stride) * component.height, 128);
    }

    JpegBits in{bytes.data() + at, bytes.size() - at};
    uint32_t since_restart = 0;
    for (uint32_t block_row = 0; block_row < down_blocks; ++block_row)
        for (uint32_t block_column = 0; block_column < across_blocks; ++block_column) {
            if (restart_interval && since_restart == restart_interval) {
                // A restart marker: the bits start again on a byte boundary
                // and every component forgets its running DC value.
                in.restart();
                while (in.at + 1 < in.length
                       && !(in.data[in.at] == 0xFF && in.data[in.at + 1] >= 0xD0 && in.data[in.at + 1] <= 0xD7))
                    ++in.at;
                if (in.at + 1 < in.length) in.at += 2;
                for (JpegComponent& component : components) component.previous = 0;
                since_restart = 0;
            }
            ++since_restart;
            for (JpegComponent& component : components)
                for (uint32_t down = 0; down < component.down; ++down)
                    for (uint32_t across = 0; across < component.across; ++across) {
                        int block[64] = {};
                        const JpegHuffman& dc = dc_tables[component.dc_table];
                        const JpegHuffman& ac = ac_tables[component.ac_table];
                        const int bits = dc.decode(in);
                        if (bits < 0 || bits > 16) { fail(error, "the file runs out part way through"); return {}; }
                        component.previous += extend(in.take(uint32_t(bits)), uint32_t(bits));
                        block[0] = component.previous * quantisers[component.quantiser][0];
                        for (int index = 1; index < 64;) {
                            const int symbol = ac.decode(in);
                            if (symbol < 0) { fail(error, "the file runs out part way through"); return {}; }
                            const int run = symbol >> 4, size = symbol & 15;
                            if (!size) {
                                if (run != 15) break;  // the rest of the block is zero
                                index += 16;
                                continue;
                            }
                            index += run;
                            if (index > 63) break;
                            block[zigzag[index]] = extend(in.take(uint32_t(size)), uint32_t(size))
                                                   * quantisers[component.quantiser][index];
                            ++index;
                        }
                        const uint32_t x = (block_column * component.across + across) * 8;
                        const uint32_t y = (block_row * component.down + down) * 8;
                        inverse_dct(block, component.pixels.data() + size_t(y) * component.stride + x, component.stride);
                    }
        }

    DecodedImage out;
    out.width = width;
    out.height = height;
    out.rgba.assign(size_t(width) * height * 4, 255);
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t column = 0; column < width; ++column) {
            const auto sample = [&](const JpegComponent& component) {
                const uint32_t x = column * component.across / widest;
                const uint32_t y = row * component.down / tallest;
                return component.pixels[size_t((std::min)(y, component.height - 1)) * component.stride
                                        + (std::min)(x, component.width - 1)];
            };
            uint8_t* const pixel = out.rgba.data() + (size_t(row) * width + column) * 4;
            if (components.size() == 1) {
                pixel[0] = pixel[1] = pixel[2] = sample(components[0]);
                continue;
            }
            // YCbCr to RGB, as JFIF defines it.
            const float y = float(sample(components[0]));
            const float blue = float(sample(components[1])) - 128.0f;
            const float red = float(sample(components[2])) - 128.0f;
            const float channels[3] = {y + 1.402f * red, y - 0.344136f * blue - 0.714136f * red, y + 1.772f * blue};
            for (int channel = 0; channel < 3; ++channel)
                pixel[channel] = uint8_t(channels[channel] < 0 ? 0 : channels[channel] > 255 ? 255 : channels[channel]);
        }
    return out;
}

DecodedImage decode_image(const std::vector<uint8_t>& bytes, std::string* error) {
    if (bytes.size() > 8 && bytes[0] == 137 && bytes[1] == 'P') return decode_png(bytes, error);
    if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8) return decode_jpeg(bytes, error);
    fail(error, "neither a PNG nor a JPEG");
    return {};
}

}
