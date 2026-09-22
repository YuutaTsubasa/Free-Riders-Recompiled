#include "image_protection.h"

namespace sfr {
ImageProtection::ImageProtection(std::span<const uint8_t> header, uint32_t base, uint32_t size) {
    const auto invalid = [&] { throw RuntimeStop("image-protection", base, "invalid or unsupported XEX page descriptors"); };
    const uint64_t image_end = uint64_t(base) + size;
    // Pinned Xenia selects this heap's page size by address, not image flags.
    if (base < 0x80000000 || base % 0x10000 || !size || size % 0x10000 || image_end > 0x90000000)
        invalid();
    const auto read = [&](size_t offset) -> uint32_t {
        if (offset > header.size() || header.size() - offset < 4) invalid();
        return uint32_t(header[offset]) << 24 | uint32_t(header[offset + 1]) << 16 |
               uint32_t(header[offset + 2]) << 8 | header[offset + 3];
    };
    if (header.size() < 24 || read(0) != 0x58455832 || read(8) != header.size()) invalid();
    const uint64_t security = read(0x10), optional_count = read(0x14);
    if (optional_count > 256 || security % 4 || security < 24 + optional_count * 8 ||
        security > header.size() || header.size() - security < 0x184) invalid();
    const uint64_t security_size = read(security), count = read(security + 0x180);
    if (!count || security_size != 0x184 + count * 24 || security_size > header.size() - security ||
        read(security + 4) != size || read(security + 0x110) != base || count > size / 0x10000) invalid();
    regions_.reserve(static_cast<size_t>(count));
    uint64_t cursor = base;
    for (uint64_t index = 0; index < count; ++index) {
        const uint32_t word = read(security + 0x184 + index * 24);
        const uint32_t type = word & 15, pages = word >> 4;
        const uint64_t end = cursor + uint64_t(pages) * 0x10000;
        if (!pages || (type != 1 && type != 2 && type != 3) || end > image_end) invalid();
        regions_.push_back({cursor, end, type == 2 ? 4u : 2u});
        cursor = end;
    }
    if (cursor != image_end) invalid();
}
bool ImageProtection::contains(uint32_t address) const {
    return !regions_.empty() && address >= regions_.front().begin && address < regions_.back().end;
}
uint32_t ImageProtection::query_address_protect(uint32_t address) const {
    for (const auto& region : regions_)
        if (address >= region.begin && address < region.end) return region.protection;
    throw RuntimeStop("memory-protection", address, "address is outside the verified XEX image");
}
}
