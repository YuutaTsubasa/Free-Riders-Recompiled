#include "runtime_shader_cache.h"
#include "guest_memory.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
void environment(const char* key, const fs::path& value) {
#ifdef _WIN32
    if (_putenv_s(key, value.string().c_str())) throw std::runtime_error("cannot set test environment");
#else
    if (setenv(key, value.string().c_str(), 1)) throw std::runtime_error("cannot set test environment");
#endif
}
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const std::string kind = argv[1];
    if (kind != "empty" && kind != "zero" && kind != "missing") return 1;
    const auto root = fs::temp_directory_path() / ("sfr-pack-" + kind + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root);
        struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
        const auto pack = root / "shaders.pack";
        if (kind != "missing") {
            std::ofstream out(pack, std::ios::binary);
            if (kind == "zero") {
                const std::array<char, 16> header{'S','F','R','S','H','P','K','2',8,0,0,0,0,0,0,0};
                out.write(header.data(), header.size());
            }
        }
        environment("SFR_SHADER_PACK", pack);
        environment("SFR_GRAPHICS", "vulkan");
        // Stop the fallback at cache creation, before any translator or GPU
        // work. Reaching this distinct error proves that missing packs remain
        // optional, while malformed existing files fail at pack validation.
        const auto blocked_cache = root / "cache-is-a-file";
        std::ofstream(blocked_cache) << 'x';
        environment("SFR_RUNTIME_SHADER_CACHE", blocked_cache);
        const std::array<uint8_t, 4> source{0,0,0,1};
        try { sfr::runtime_shader(sfr::ShaderStage::vertex, source); }
        catch (const sfr::RuntimeStop& stop) {
            const bool correct = kind == "missing"
                ? stop.category == "runtime-shader" && stop.detail.find("cannot create") != std::string::npos
                : stop.category == "shader-pack";
            if (!correct) throw std::runtime_error("unexpected stop: " + stop.category + ": " + stop.detail);
            std::cout << kind << " pack handling passed\n";
            return 0;
        }
        throw std::runtime_error("expected pack or cache validation to stop");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
