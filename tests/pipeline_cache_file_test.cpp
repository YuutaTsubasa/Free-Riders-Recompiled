#include "pipeline_cache_file.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void write(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
}
std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void checks(const std::filesystem::path& root) {
    const auto file = root / "cache.bin";
    require(sfr::load_pipeline_cache_file(file).empty(), "missing cache is a miss");
    const std::vector<uint8_t> first{0, 1, 2, 255, 128};
    require(sfr::save_pipeline_cache_file(file, first), "first cache save succeeds");
    require(sfr::load_pipeline_cache_file(file) == first, "opaque bytes survive disk round trip");
    const std::vector<uint8_t> second{21, 0, 254};
    require(sfr::save_pipeline_cache_file(file, second), "existing cache can be replaced");
    require(sfr::load_pipeline_cache_file(file) == second, "replacement preserves new bytes");
    const auto good = read(file);
    for (size_t n = 0; n < good.size(); ++n) {
        write(file, {good.begin(), good.begin() + n});
        require(sfr::load_pipeline_cache_file(file).empty(), "every truncated file is rejected");
    }
    for (size_t i = 0; i < good.size(); ++i) {
        auto corrupt = good;
        corrupt[i] ^= 1;
        write(file, corrupt);
        require(sfr::load_pipeline_cache_file(file).empty(), "header and payload corruption is rejected");
    }
    auto appended = good;
    appended.push_back(0);
    write(file, appended);
    require(sfr::load_pipeline_cache_file(file).empty(), "trailing data is rejected");
    write(file, good);
    require(!sfr::save_pipeline_cache_file(file, {}), "empty cache is not persisted");
    require(!sfr::save_pipeline_cache_file(file, std::vector<uint8_t>(sfr::pipeline_cache_max_bytes + 1)),
            "oversize cache is not persisted");
    require(read(file) == good, "rejected saves preserve the last good cache");
    const auto directory = root / "destination-directory";
    std::filesystem::create_directory(directory);
    write(directory / "keep", first);
    require(!sfr::save_pipeline_cache_file(directory, second), "failed replacement is reported");
    require(read(directory / "keep") == first, "failed replacement preserves destination");

    sfr::PipelineCacheIdentity identity{0x5143, 0x750, {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}};
    std::vector<uint8_t> data{32,0,0,0, 1,0,0,0, 0x43,0x51,0,0, 0x50,7,0,0};
    data.insert(data.end(), identity.uuid.begin(), identity.uuid.end());
    data.push_back(99);
    require(sfr::compatible_pipeline_cache(data, identity), "matching Vulkan header is accepted");
    for (size_t i = 0; i < 32; ++i) {
        auto wrong = data;
        wrong[i] ^= 0x80;
        require(!sfr::compatible_pipeline_cache(wrong, identity), "incompatible Vulkan header is rejected");
    }
    data.resize(31);
    require(!sfr::compatible_pipeline_cache(data, identity), "truncated Vulkan header is rejected");
}
}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("sfr-pipeline-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    int result = 0;
    try { checks(root); std::cout << "Pipeline cache file checks passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    std::filesystem::remove_all(root);
    return result;
}
