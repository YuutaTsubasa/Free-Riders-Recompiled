#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sfr {

// A decoded picture, four bytes a pixel, red first, rows from the top.
struct DecodedImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    explicit operator bool() const { return width && height && rgba.size() == size_t(width) * height * 4; }
};

// PNG and JPEG, which is what a .vrm keeps its textures as. Written here
// rather than taken as a dependency: between them these are about six hundred
// lines, and the alternative was adding zlib and a JPEG library to a build
// that has none.
//
// What is not read is said plainly rather than guessed at, and `error` is told
// why: an interlaced PNG and a progressive JPEG both come back empty.
DecodedImage decode_png(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
DecodedImage decode_jpeg(const std::vector<uint8_t>& bytes, std::string* error = nullptr);

// Whichever of the two the bytes are, by what they start with.
DecodedImage decode_image(const std::vector<uint8_t>& bytes, std::string* error = nullptr);

// DEFLATE, as a PNG's pixels are stored (RFC 1951, with the two byte zlib
// header RFC 1950 puts in front). Exposed for its own test.
bool inflate(const uint8_t* bytes, size_t length, std::vector<uint8_t>& out, bool zlib_header = true);

}
