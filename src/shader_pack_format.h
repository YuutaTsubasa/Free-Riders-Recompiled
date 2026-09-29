#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace sfr {
// Keep in sync with scripts/shader_pack_format.py and the runtime cache key.
inline constexpr uint32_t shader_abi_version = 9;
inline constexpr size_t shader_pack_header_size = 16;
inline uint32_t shader_pack_count(std::span<const uint8_t> bytes) {
    if (bytes.size() < shader_pack_header_size ||
        std::string_view(reinterpret_cast<const char*>(bytes.data()), 8) != "SFRSHPK2")
        throw std::invalid_argument("outdated or invalid shader pack; reinstall the complete matching release");
    const auto word = [&](size_t at) {
        return uint32_t(bytes[at]) | uint32_t(bytes[at+1]) << 8 |
               uint32_t(bytes[at+2]) << 16 | uint32_t(bytes[at+3]) << 24;
    };
    if (word(8) != shader_abi_version)
        throw std::invalid_argument("shader pack ABI mismatch; reinstall the complete matching release");
    const uint32_t count = word(12);
    if (!count) throw std::invalid_argument("shader pack contains no shaders; reinstall the complete matching release");
    return count;
}
}
