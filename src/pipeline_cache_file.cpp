#include "pipeline_cache_file.h"
#include <algorithm>
#include <fstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace sfr {
namespace {
constexpr std::array<uint8_t, 8> magic{'S','F','R','P','S','O',0,1};
uint64_t read_le(const uint8_t* bytes, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}
void write_le(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) bytes[i] = uint8_t(value >> (8 * i));
}
uint64_t checksum(std::span<const uint8_t> data) {
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : data) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}
}
bool compatible_pipeline_cache(std::span<const uint8_t> data, const PipelineCacheIdentity& identity) {
    // Vulkan's version-one header is specified in little endian, not host ABI.
    return data.size() >= 32 && read_le(data.data(), 4) == 32 &&
        read_le(data.data() + 4, 4) == 1 && read_le(data.data() + 8, 4) == identity.vendor &&
        read_le(data.data() + 12, 4) == identity.device &&
        std::equal(identity.uuid.begin(), identity.uuid.end(), data.begin() + 16);
}
std::vector<uint8_t> load_pipeline_cache_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto size = in.tellg();
    if (size <= 24 || size > std::streamoff(pipeline_cache_max_bytes + 24)) return {};
    in.seekg(0);
    std::array<uint8_t, 24> header{};
    if (!in.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        !std::equal(magic.begin(), magic.end(), header.begin()) ||
        read_le(header.data() + 8, 8) != uint64_t(size - std::streamoff(24))) return {};
    std::vector<uint8_t> data(size_t(size) - 24);
    if (!in.read(reinterpret_cast<char*>(data.data()), data.size()) ||
        checksum(data) != read_le(header.data() + 16, 8)) return {};
    return data;
}
bool save_pipeline_cache_file(const std::filesystem::path& path, std::span<const uint8_t> data) {
    if (data.empty() || data.size() > pipeline_cache_max_bytes) return false;
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto temporary = path;
    temporary += ".new";
    std::array<uint8_t, 24> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    write_le(header.data() + 8, data.size());
    write_le(header.data() + 16, checksum(data));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(header.data()), header.size());
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
        if (!out) return false;
    }
    // Never remove the last good file first. An interrupted write leaves it intact.
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::filesystem::rename(temporary, path, error);
    return !error;
#endif
}
}
