#include "image_decode.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void put_big_endian(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(uint8_t(value >> shift));
}

void put_chunk(std::vector<uint8_t>& bytes, const char* type, const std::vector<uint8_t>& data) {
    put_big_endian(bytes, uint32_t(data.size()));
    for (int index = 0; index < 4; ++index) bytes.push_back(uint8_t(type[index]));
    bytes.insert(bytes.end(), data.begin(), data.end());
    put_big_endian(bytes, 0);  // the checksum, which nothing here reads
}

// Deflate that does not compress: a single stored block, which is a real
// stream a real decoder has to handle.
std::vector<uint8_t> stored_deflate(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out = {0x78, 0x01, 0x01};
    out.push_back(uint8_t(data.size()));
    out.push_back(uint8_t(data.size() >> 8));
    out.push_back(uint8_t(~data.size()));
    out.push_back(uint8_t(~data.size() >> 8));
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), {0, 0, 0, 0});  // the adler sum, likewise unread
    return out;
}

// Two rows of two pixels, the second row written as differences from the
// first, which is what a PNG's "up" filter means.
std::vector<uint8_t> two_by_two_png(const std::vector<uint8_t>& compressed) {
    std::vector<uint8_t> header;
    put_big_endian(header, 2);
    put_big_endian(header, 2);
    header.insert(header.end(), {8, 2, 0, 0, 0});  // eight bits, colour type 2, no interlace
    std::vector<uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
    put_chunk(out, "IHDR", header);
    put_chunk(out, "IDAT", compressed);
    put_chunk(out, "IEND", {});
    return out;
}

std::vector<uint8_t> two_by_two_png() {
    return two_by_two_png(stored_deflate({
        0, 255, 0, 0, 0, 255, 0,   // filter 0: red, green
        2, 1, 2, 3, 4, 5, 6,       // filter 2: each byte plus the one above it
    }));
}

void a_png_comes_back() {
    std::string error;
    const sfr::DecodedImage image = sfr::decode_png(two_by_two_png(), &error);
    require(bool(image), error.empty() ? "the PNG reads" : error.c_str());
    require(image.width == 2 && image.height == 2, "two by two");
    require(image.rgba[0] == 255 && image.rgba[1] == 0 && image.rgba[3] == 255, "red, and opaque");
    require(image.rgba[4] == 0 && image.rgba[5] == 255, "then green");
    // The second row is the first plus its own bytes, wrapping round, which
    // is what its filter says: 255 + 1 is 0, and 255 + 5 is 4.
    require(image.rgba[8] == 0 && image.rgba[9] == 2 && image.rgba[10] == 3, "the up filter is undone");
    require(image.rgba[12] == 4 && image.rgba[13] == 4 && image.rgba[14] == 6, "on every byte of it");
}

void what_a_png_refuses() {
    std::string error;
    require(!sfr::decode_png({1, 2, 3}, &error) && !error.empty(), "something that is not a PNG is refused");
    std::vector<uint8_t> interlaced = two_by_two_png();
    interlaced[8 + 8 + 12] = 1;  // the interlace byte of IHDR
    require(!sfr::decode_png(interlaced, &error) && error.find("interlaced") != std::string::npos,
            "an interlaced PNG says that is what it is");
}

// A stream that really was compressed, by a real compressor, so that the
// Huffman path is exercised and not just the stored one.
void a_compressed_stream_comes_back() {
    const std::vector<uint8_t> compressed = {
        120, 218, 93, 137, 199, 1, 128, 32, 12, 69, 87, 249, 3, 184, 84, 148, 136, 177, 36, 10, 193,
        54, 189, 232, 209, 219, 43, 62, 48, 182, 34, 221, 132, 54, 217, 161, 232, 237, 196, 88, 150,
        53, 195, 118, 78, 240, 186, 103, 186, 47, 4, 139, 13, 72, 195, 91, 180, 26, 103, 136, 131,
        34, 137, 126, 249, 71, 15, 107, 139, 31, 22};
    std::vector<uint8_t> out;
    require(sfr::inflate(compressed.data(), compressed.size(), out), "the stream inflates");
    const std::string text(out.begin(), out.end());
    require(text == "the quick brown fox jumps over the lazy dog, and then does it again and again and again",
            "and comes back word for word");
}

// One grey eight by eight block: a quantiser of ones, a DC code that says
// seven more bits, those seven bits, and an end-of-block. The flat value 80
// through the inverse transform is 80/8 = 10 above the middle grey of 128.
std::vector<uint8_t> flat_grey_jpeg() {
    std::vector<uint8_t> out = {0xFF, 0xD8};
    std::vector<uint8_t> quantiser = {0xFF, 0xDB, 0, 67, 0};
    for (int index = 0; index < 64; ++index) quantiser.push_back(1);
    out.insert(out.end(), quantiser.begin(), quantiser.end());
    const std::vector<uint8_t> frame = {0xFF, 0xC0, 0, 11, 8, 0, 8, 0, 8, 1, 1, 0x11, 0};
    out.insert(out.end(), frame.begin(), frame.end());
    for (const uint8_t kind : {uint8_t(0x00), uint8_t(0x10)}) {
        std::vector<uint8_t> table = {0xFF, 0xC4, 0, 20, kind, 1};
        for (int length = 2; length <= 16; ++length) table.push_back(0);
        table.push_back(kind ? 0x00 : 7);  // end of block, or a seven bit value
        out.insert(out.end(), table.begin(), table.end());
    }
    const std::vector<uint8_t> scan = {0xFF, 0xDA, 0, 8, 1, 1, 0x00, 0, 63, 0, 0x50, 0x7F, 0xFF, 0xD9};
    out.insert(out.end(), scan.begin(), scan.end());
    return out;
}

void a_jpeg_comes_back() {
    std::string error;
    const sfr::DecodedImage image = sfr::decode_jpeg(flat_grey_jpeg(), &error);
    require(bool(image), error.empty() ? "the JPEG reads" : error.c_str());
    require(image.width == 8 && image.height == 8, "eight by eight");
    for (size_t pixel = 0; pixel < 64; ++pixel) {
        const uint8_t grey = image.rgba[pixel * 4];
        require(grey >= 137 && grey <= 139, "every pixel is the grey the block asks for");
        require(image.rgba[pixel * 4 + 1] == grey && image.rgba[pixel * 4 + 3] == 255, "grey, and opaque");
    }
}

void what_a_jpeg_refuses() {
    std::string error;
    require(!sfr::decode_jpeg({1, 2, 3}, &error) && !error.empty(), "something that is not a JPEG is refused");
    std::vector<uint8_t> progressive = flat_grey_jpeg();
    for (size_t at = 0; at + 1 < progressive.size(); ++at)
        if (progressive[at] == 0xFF && progressive[at + 1] == 0xC0) { progressive[at + 1] = 0xC2; break; }
    require(!sfr::decode_jpeg(progressive, &error) && error.find("progressive") != std::string::npos,
            "a progressive JPEG says that is what it is");
}

void jpeg_segments_must_fit_their_declared_length() {
    // Keep the physical payload intact but shorten its declared segment, so
    // rejection must happen at the segment boundary, not merely end of file.
    const uint8_t shortened[][2] = {
        {0xC0, 3}, {0xC0, 8},  // the frame header, then its component list
        {0xC4, 3}, {0xC4, 19}, // the table header, then its symbols
        {0xDB, 3}, {0xDB, 66}, // the quantiser's values
        {0xDA, 3}, {0xDA, 7},  // the scan component list and its trailing fields
    };
    for (const auto& segment : shortened) {
        auto bytes = flat_grey_jpeg();
        bool found = false;
        for (size_t at = 2; at + 3 < bytes.size(); ++at) {
            if (bytes[at] != 0xFF || bytes[at + 1] != segment[0]) continue;
            bytes[at + 2] = 0;
            bytes[at + 3] = segment[1];
            found = true;
            break;
        }
        require(found, "the fixture contains the segment to truncate");
        std::string error;
        require(!sfr::decode_jpeg(bytes, &error) && !error.empty(), "a short JPEG segment is refused");
    }
    auto restart = flat_grey_jpeg();
    restart.insert(restart.begin() + 2, {0xFF, 0xDD, 0, 2});
    require(!sfr::decode_jpeg(restart), "a restart segment needs two payload bytes");
    for (const uint8_t marker : {uint8_t(0xC0), uint8_t(0xC1), uint8_t(0xC4), uint8_t(0xDB), uint8_t(0xDD), uint8_t(0xDA)}) {
        std::string error;
        require(!sfr::decode_jpeg({0xFF, 0xD8, 0xFF, marker, 0, 2}, &error) && !error.empty(),
                "a JPEG ending at an empty segment is refused");
    }
    for (const uint8_t marker : {uint8_t(0xC4), uint8_t(0xDB)})
        require(!sfr::decode_jpeg({0xFF, 0xD8, 0xFF, marker, 0, 3, 0}),
                "a JPEG table identifier without its table is refused");
    auto wide_quantiser = flat_grey_jpeg();
    wide_quantiser[6] = 0x10;
    require(!sfr::decode_jpeg(wide_quantiser), "a sixteen-bit quantiser needs twice as many value bytes");
}

void png_inflation_cannot_exceed_the_header() {
    std::string error;
    require(!sfr::decode_png(two_by_two_png(stored_deflate(std::vector<uint8_t>(15, 0))), &error)
                && !error.empty(), "stored output beyond the fourteen scanline bytes is refused");
    // zlib streams containing sixty-four zeros (a back-reference) and fifteen
    // mostly distinct bytes (literals), with valid filter bytes in both rows.
    require(!sfr::decode_png(two_by_two_png({120, 156, 99, 96, 160, 12, 0, 0, 0, 64, 0, 1})),
            "a back-reference cannot expand beyond the scanlines");
    require(!sfr::decode_png(two_by_two_png({120, 156, 99, 96, 100, 98, 102, 97, 101, 99, 224, 224,
                                          228, 226, 230, 225, 229, 3, 0, 2, 7, 0, 99})),
            "a literal cannot expand beyond the scanlines");
    require(bool(sfr::decode_png(two_by_two_png(stored_deflate(std::vector<uint8_t>(14, 0))))),
            "output exactly matching the scanlines is accepted");
}

void the_right_decoder_is_chosen() {
    require(bool(sfr::decode_image(two_by_two_png())), "a PNG is recognised by what it starts with");
    require(bool(sfr::decode_image(flat_grey_jpeg())), "and so is a JPEG");
    std::string error;
    require(!sfr::decode_image({0, 1, 2, 3, 4, 5, 6, 7, 8}, &error) && !error.empty(), "and neither is refused");
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string test = argv[1];
            if (test == "segments") jpeg_segments_must_fit_their_declared_length();
            else if (test == "inflate") png_inflation_cannot_exceed_the_header();
            else throw std::runtime_error("unknown test");
            return 0;
        }
        a_png_comes_back();
        what_a_png_refuses();
        a_compressed_stream_comes_back();
        a_jpeg_comes_back();
        what_a_jpeg_refuses();
        the_right_decoder_is_chosen();
        jpeg_segments_must_fit_their_declared_length();
        png_inflation_cannot_exceed_the_header();
        std::cout << "image decoding checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
