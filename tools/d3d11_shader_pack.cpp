// sfr_d3d11_shader_pack <shaders.pack> <shaders-d3d11.pack> [work-directory]
//
// Makes the Direct3D 11 backend's shader pack (src/d3d11_shader_pack.h,
// Issue #359) from a release's shaders.pack, on Windows, in the checkout
// (it runs the pinned translator, out/tools/shader-translator, as the game
// does): every shader's source container is translated and rewritten for
// shader model 5.0 exactly as a D3D11 run of the game does
// (runtime_shader), then compiled with d3dcompiler_47 as the backend
// compiles it (plume::D3D11CompileShader), for the specializations pipelines
// use: 0, and the alpha test's 2 where the pixel shader has it.
//
// The output holds code taken from the game: like shaders.pack, keep it in
// out/ and in releases only.
#include "d3d11_shader_pack.h"
#include "guest_memory.h"
#include "native_graphics.h"
#include "plume_d3d11.h"
#include "runtime_shader_cache.h"
#include "shader_pack_format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: sfr_d3d11_shader_pack <shaders.pack> <shaders-d3d11.pack> [work-directory]\n";
        return 2;
    }
    try {
        namespace fs = std::filesystem;
        const fs::path work = fs::absolute(argc > 3 ? fs::path(argv[3]) : fs::path("out/shaders/d3d11-pack-work"));
        fs::create_directories(work);
        const auto environment = [](const char* name, const std::string& value) {
            if (_putenv_s(name, value.c_str())) throw std::runtime_error("cannot set shader environment");
        };
        // A D3D11 run's translation, into a cache of its own, with neither
        // pack consulted.
        environment("SFR_GRAPHICS", "d3d11");
        environment("SFR_RUNTIME_SHADER_CACHE", (work / "runtime").string());
        environment("SFR_SHADER_PACK", (work / "no-shaders.pack").string());
        environment("SFR_D3D11_SHADER_PACK", (work / "no-shaders-d3d11.pack").string());
        if (sfr::selected_graphics_backend() != sfr::GraphicsBackend::d3d11) throw std::runtime_error("not a D3D11 run");

        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open the shader pack");
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        const uint32_t count = sfr::shader_pack_count(bytes);
        size_t at = sfr::shader_pack_header_size;
        const auto word = [&]() {
            if (bytes.size() - at < 4) throw std::runtime_error("the shader pack is truncated");
            const uint32_t value = uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
                                   uint32_t(bytes[at + 3]) << 24;
            at += 4;
            return value;
        };
        std::vector<sfr::D3D11PackedShader> shaders;
        size_t skipped = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t stage = word();
            word();  // the mask, as this translation reports it
            const uint32_t source = word(), dxil = word(), spirv = word();
            if (stage > 1 || uint64_t(source) + dxil + spirv > bytes.size() - at)
                throw std::runtime_error("the shader pack has an invalid entry");
            const std::span<const uint8_t> container(bytes.data() + at, source);
            at += size_t(source) + dxil + spirv;
            const auto& entry = sfr::runtime_shader(sfr::ShaderStage(stage), container);
            const auto hlsl = entry.code(false);
            if (hlsl.empty()) {
                ++skipped;  // RUNTIME_SHADER says why; the game cannot draw with it either
                continue;
            }
            sfr::D3D11PackedShader shader;
            shader.stage = stage;
            shader.mask = entry.specialization_mask;
            shader.source.assign(container.begin(), container.end());
            shader.hlsl.assign(hlsl.begin(), hlsl.end());
            shader.variants.push_back({0, {}});
            // The pipelines' specializations (guest_graphics_hooks.cpp: the alpha test).
            if (stage == 1 && (shader.mask & 2)) shader.variants.push_back({2, {}});
            shaders.push_back(std::move(shader));
            std::cout << "SHADER_TRANSLATED " << shaders.size() + skipped << '/' << count << '\n';
        }
        if (at != bytes.size()) throw std::runtime_error("the shader pack has trailing data");

        // d3dcompiler on every core: some shaders take seconds.
        struct Job {
            sfr::D3D11PackedShader* shader;
            sfr::D3D11PackedShader::Variant* variant;
        };
        std::vector<Job> jobs;
        for (auto& shader : shaders)
            for (auto& variant : shader.variants) jobs.push_back({&shader, &variant});
        std::atomic<size_t> next{0}, failed{0};
        std::mutex output;
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < (std::max)(1u, std::thread::hardware_concurrency()); ++t)
            threads.emplace_back([&] {
                for (size_t j; (j = next++) < jobs.size();) {
                    auto& [shader, variant] = jobs[j];
                    std::string errors;
                    variant->dxbc = plume::D3D11CompileShader(shader->hlsl, "shaderMain",
                                                              shader->stage == 0 ? "vs_5_0" : "ps_5_0", variant->spec,
                                                              &errors);
                    if (variant->dxbc.empty()) {
                        ++failed;
                        std::lock_guard guard(output);
                        std::cerr << "D3D11_COMPILE failed=1 stage=" << shader->stage << " source_bytes=" << shader->source.size() << " spec=" << variant->spec
                                  << ' ' << errors.substr(0, 400) << '\n';
                    }
                }
            });
        for (auto& thread : threads) thread.join();
        if (failed) throw std::runtime_error(std::to_string(failed.load()) + " shader variants did not compile");
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        const auto packed = sfr::encode_d3d11_shader_pack(shaders);
        sfr::decode_d3d11_shader_pack(packed);
        const fs::path output_path = argv[2];
        if (!output_path.parent_path().empty()) fs::create_directories(output_path.parent_path());
        fs::path temporary = output_path;
        temporary += ".new";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(packed.size()));
            if (!out) throw std::runtime_error("cannot write " + temporary.string());
        }
        fs::rename(temporary, output_path);
        std::cout << output_path.string() << ": " << shaders.size() << " shaders (" << skipped << " untranslatable left out), "
                  << jobs.size() << " compiled in " << seconds << " s, " << packed.size() << " bytes\n";
    } catch (const sfr::RuntimeStop& error) {
        std::cerr << error.category << ": " << error.detail << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
