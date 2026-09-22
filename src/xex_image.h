#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace sfr {
// The supported default.xex (Sonic Free Riders, USA/Europe disc).
constexpr size_t supported_xex_size = 14241792;
// Its exact size and SHA-1 fingerprint.
bool is_supported_xex(std::span<const uint8_t> bytes);

struct DecodedImage {
    uint32_t base, entry_point, size;
    size_t import_functions;
};
// Writes the runtime's image directory (image.bin, xex_header.bin, the
// .tsv tables, then complete.txt) into output, which must not exist.
// Throws on any other input or a failed write.
DecodedImage write_image_dump(std::span<const uint8_t> xex, const std::filesystem::path& output);
}
