// Recompile source containers from a known pack into the current runtime ABI.
// Bytecode in the input is never loaded or copied to the output cache.
#include "runtime_shader_cache.h"
#include "native_graphics.h"
#include "guest_memory.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: sfr_shader_cache_rebuild input.pack new-cache-directory\n"
                     "Run once per backend using SFR_GRAPHICS=vulkan or d3d12.\n";
        return 2;
    }
    try {
        namespace fs = std::filesystem;
        const fs::path cache = fs::absolute(argv[2]);
        fs::create_directories(cache);
        const auto environment = [](const char* name, const std::string& value) {
#ifdef _WIN32
            if (_putenv_s(name, value.c_str())) throw std::runtime_error("cannot set shader environment");
#else
            if (setenv(name, value.c_str(), 1)) throw std::runtime_error("cannot set shader environment");
#endif
        };
        const fs::path absent = cache / "source-only-no-input-binaries.pack";
        if (fs::exists(absent)) throw std::runtime_error("source-only shader path must not exist");
        environment("SFR_RUNTIME_SHADER_CACHE", cache.string());
        environment("SFR_SHADER_PACK", absent.string());
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open source shader pack");
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        size_t at = 0;
        const auto word = [&]() {
            if (bytes.size() - at < 4) throw std::runtime_error("truncated source shader pack");
            const uint32_t value = uint32_t(bytes[at]) | uint32_t(bytes[at+1]) << 8 |
                uint32_t(bytes[at+2]) << 16 | uint32_t(bytes[at+3]) << 24;
            at += 4; return value;
        };
        if (bytes.size() < 16 || std::string_view(reinterpret_cast<const char*>(bytes.data()),8) != "SFRSHPK2")
            throw std::runtime_error("unrecognized source shader pack");
        at = 8;
        const uint32_t abi = word(), count = word();
        if ((abi != 8 && abi != 9) || !count || count > (bytes.size() - at) / 24)
            throw std::runtime_error("unsupported source ABI or invalid shader count");
        struct Source { sfr::ShaderStage stage; std::span<const uint8_t> data; };
        std::vector<Source> sources;
        for (uint32_t i = 0; i < count; ++i) {
            const auto stage = word(); word();
            const uint32_t source_bytes = word(), dxil = word(), spirv = word();
            const uint64_t total = uint64_t(source_bytes) + dxil + spirv;
            if (stage > 1 || source_bytes < 4 || total > bytes.size() - at)
                throw std::runtime_error("invalid source shader record");
            const auto actual_stage = bytes[at + 3] & 1 ? sfr::ShaderStage::vertex : sfr::ShaderStage::pixel;
            if (actual_stage != static_cast<sfr::ShaderStage>(stage)) throw std::runtime_error("source stage mismatch");
            sources.push_back({actual_stage, std::span(bytes).subspan(at, source_bytes)});
            at += size_t(total);
        }
        if (at != bytes.size()) throw std::runtime_error("source shader pack has trailing data");
        const bool vulkan = sfr::selected_graphics_backend() == sfr::GraphicsBackend::vulkan;
        uint32_t complete = 0;
        for (const auto& source : sources) {
            const auto& compiled = sfr::runtime_shader(source.stage, source.data);
            if (compiled.code(vulkan).empty()) throw std::runtime_error("shader compilation failed; see runtime shader diagnostic");
            std::cout << "SHADER_REBUILT " << ++complete << '/' << sources.size() << '\n';
        }
        std::cout << "Rebuilt " << complete << " shader sources for " << (vulkan ? "Vulkan" : "D3D12") << '\n';
    } catch (const sfr::RuntimeStop& error) {
        std::cerr << error.category << ": " << error.detail << '\n'; return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
