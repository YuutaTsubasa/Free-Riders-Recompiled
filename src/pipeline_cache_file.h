#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sfr {
constexpr size_t pipeline_cache_max_bytes = 64 * 1024 * 1024;
struct PipelineCacheIdentity {
    uint32_t vendor, device;
    std::array<uint8_t, 16> uuid;
};
bool compatible_pipeline_cache(std::span<const uint8_t> data, const PipelineCacheIdentity& identity);
std::vector<uint8_t> load_pipeline_cache_file(const std::filesystem::path& path);
bool save_pipeline_cache_file(const std::filesystem::path& path, std::span<const uint8_t> data);
}
