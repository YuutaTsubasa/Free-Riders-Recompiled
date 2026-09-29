#include "native_shaders.h"
#include "native_graphics.h"
#include "guest_memory.h"
#include "runtime_shader_cache.h"
#include "shader_pack_format.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
// Valid pixel container header, with no executable instructions needed: its
// specialized-library payload is retained, never linked or submitted to GPU.
constexpr std::array<uint8_t,40> original{0x10,0x2A,0x11,0, 0,0,0,36, 0,0,0,4};
const sfr::ShaderCacheEntry stale{original, {}, sfr::ShaderStage::pixel, 2};
}
// Override the archive's generated prepared cache with a matching stale entry.
namespace sfr { std::span<const ShaderCacheEntry> compiled_shader_cache() { return std::span(&stale,1); } }
int main() {
#ifndef _WIN32
    return 0; // The prepared-cache hazard exists on the D3D12 path only.
#else
    try {
        _putenv_s("SFR_GRAPHICS","d3d12");
        namespace fs = std::filesystem;
        const auto folder = fs::temp_directory_path() / ("sfr-shader-abi-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(folder);
        struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path,error); } } cleanup{folder};
        const auto pack = folder / "current.pack";
        {
            std::ofstream output(pack,std::ios::binary);
            output.write("SFRSHPK2",8);
            const auto word = [&](uint32_t n) { for (int i=0;i<4;++i) output.put(char(n>>(i*8))); };
            word(sfr::shader_abi_version); word(1);
            word(1); word(2); word(original.size()); word(1); word(0);
            output.write(reinterpret_cast<const char*>(original.data()), original.size());
            output.put(char(0xA9));
        }
        _putenv_s("SFR_SHADER_PACK",pack.string().c_str());
        sfr::GuestMemory memory;
        sfr::NativeGraphics graphics;
        graphics.initialize();
        if (graphics.backend()!=sfr::GraphicsBackend::d3d12) { std::cout << "D3D12 unavailable; ABI selection test skipped\n"; return 0; }
        sfr::NativeShaders shaders(memory,graphics);
        constexpr uint32_t source = 0x10000000;
        memory.map(source,4096);
        for (size_t i=0;i<original.size();++i) memory.store<uint8_t>(source+i,original[i]);
        sfr::runtime_shader_translation=true;
        const auto handle=shaders.create(sfr::ShaderStage::pixel,source);
        const auto& shader=shaders.get(handle);
        if (shader.entry==&stale || shader.entry->dxil.size()!=1 || shader.entry->dxil[0]!=0xA9 || shader.shader)
            throw std::runtime_error("runtime mode must select current ABI pack before stale prepared cache");
        std::cout << "Current shader ABI wins over matching stale prepared cache\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
#endif
}
