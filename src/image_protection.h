#pragma once
#include "guest_memory.h"
#include <span>

namespace sfr {
// Immutable query metadata for the current 64 KiB XEX image profile.
class ImageProtection {
public:
    ImageProtection(std::span<const uint8_t> header, uint32_t base, uint32_t size);
    bool contains(uint32_t address) const;
    uint32_t query_address_protect(uint32_t address) const;
private:
    struct Region { uint64_t begin, end; uint32_t protection; };
    std::vector<Region> regions_;
};
}
