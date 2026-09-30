#include "guest_memory.h"
#include "native_graphics.h"
#include "native_presentation.h"
#include "native_renderer.h"
#include "pipeline_manifest.h"
#include "runtime_shader_cache.h"
#include "shader_pack_format.h"
#include "plume_render_interface.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void environment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
std::string quote(const fs::path& path) {
#ifdef _WIN32
    return "\"" + path.string() + "\"";
#else
    std::string result = "'";
    for (char c : path.string()) result += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return result + "'";
#endif
}
std::vector<uint8_t> compile(const fs::path& directory, bool vulkan, bool vertex) {
    const auto source = directory / (vertex ? "vertex.hlsl" : "pixel.hlsl");
    const auto output = directory / (vertex ? "vertex.bin" : "pixel.bin");
    // No private assets, vertex buffers, resource descriptors or shader header
    // dependencies: a full-screen triangle and a distinctive constant color.
    std::ofstream(source) << (vertex ? R"(
float4 shaderMain(uint id : SV_VertexID) : SV_Position {
    float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
    return float4(p[id], 0.5, 1);
})" : R"(
float4 shaderMain() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
)");
    const char* override = std::getenv("SFR_DXC");
#ifdef _WIN32
    constexpr const char* compiler = "tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe";
#elif defined(__APPLE__)
    constexpr const char* compiler = "tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc-macos";
#else
    constexpr const char* compiler = "tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc-linux";
#endif
    const fs::path dxc = override ? fs::path(override) : fs::path(SFR_SOURCE_DIR) / compiler;
    const std::string command = quote(dxc) + " -T " + (vertex ? "vs_6_0" : "ps_6_0") +
        " -E shaderMain -HV 2021 -all-resources-bound -Qstrip_debug" +
        (vulkan ? " -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.2" : "") +
        (vulkan && vertex ? " -fvk-invert-y" : "") + " -Fo " + quote(output) + " " + quote(source) +
        " > " + quote(directory / "compile.log") + " 2>&1";
#ifdef _WIN32
    const int status = std::system(("\"" + command + "\"").c_str());
#else
    const char* library_override = std::getenv("SFR_DXC_LIBRARY");
    const fs::path libraries = library_override ? fs::path(library_override) :
        fs::path(SFR_SOURCE_DIR) / "tools/XenosRecomp/thirdparty/dxc-bin/lib/x64";
#ifdef __APPLE__
    const std::string library_environment = "DYLD_LIBRARY_PATH=";
#else
    const std::string library_environment = "LD_LIBRARY_PATH=";
#endif
    const int status = std::system((library_environment + quote(libraries) + " " + command).c_str());
#endif
    if (status) throw std::runtime_error(read(directory / "compile.log"));
    const auto code = read(output);
    require(!code.empty(), "DXC fixture bytecode is nonempty");
    return {code.begin(), code.end()};
}
// Valid empty Xenos containers, differing in the vertex/pixel stage flag.
// The pack supplies actual compiled fixture bytecode; translation is forbidden.
constexpr std::array<uint8_t, 40> vertex_source{0x10,0x2A,0x11,1, 0,0,0,36, 0,0,0,4};
constexpr std::array<uint8_t, 40> pixel_source{0x10,0x2A,0x11,0, 0,0,0,36, 0,0,0,4};
void word(std::ostream& output, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) output.put(char(value >> (8 * i)));
}
void write_pack(const fs::path& path, bool vulkan, const std::vector<uint8_t>& vertex,
                const std::vector<uint8_t>& pixel) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write("SFRSHPK2", 8);
    word(output, sfr::shader_abi_version);
    word(output, 2);
    const auto entry = [&](sfr::ShaderStage stage, std::span<const uint8_t> source,
                           std::span<const uint8_t> code) {
        word(output, uint32_t(stage)); word(output, 0); word(output, uint32_t(source.size()));
        word(output, vulkan ? 0 : uint32_t(code.size()));
        word(output, vulkan ? uint32_t(code.size()) : 0);
        output.write(reinterpret_cast<const char*>(source.data()), std::streamsize(source.size()));
        output.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size()));
    };
    entry(sfr::ShaderStage::vertex, vertex_source, vertex);
    entry(sfr::ShaderStage::pixel, pixel_source, pixel);
    output.close();
    require(bool(output), "fixture shader pack is written");
}
std::unique_ptr<plume::RenderShader> live_shader(sfr::NativeGraphics& graphics,
                                               const sfr::ShaderCacheEntry& entry) {
    const bool vulkan = graphics.backend() == sfr::GraphicsBackend::vulkan;
    const auto code = entry.code(vulkan);
    auto shader = graphics.device().createShader(code.data(), code.size(), "shaderMain",
        vulkan ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL);
    require(bool(shader), "independent live shader is created");
    return shader;
}
void expect_color(std::span<const uint8_t> pixels, bool red_only) {
    require(pixels.size() == 16 * 16 * 4, "fixture readback has expected dimensions");
    for (size_t at = 0; at < pixels.size(); at += 4) {
        require(std::abs(int(pixels[at]) - (red_only ? 0 : 191)) <= 1 &&
                std::abs(int(pixels[at + 1]) - (red_only ? 0 : 128)) <= 1 &&
                std::abs(int(pixels[at + 2]) - 64) <= 1 && pixels[at + 3] == 255,
                "every readback pixel matches the pipeline's color and write mask");
    }
}
void clear(sfr::NativePresentation& presentation) {
    presentation.clear(sfr::NativeClear{.color=true, .color_value={0,0,0,1}});
}
void screenshot(sfr::NativeGraphics& graphics, const fs::path& path) {
    sfr::NativePresentation presentation(graphics, 640, 360);
    require(presentation.preparation_progress(120, 300), "startup UI accepts progress");
    const auto pixels = presentation.readback_color();
    require(pixels.size() == 640 * 360 * 4, "startup UI readback has expected dimensions");
    require(std::any_of(pixels.begin(), pixels.end(), [](uint8_t value) { return value > 128 && value < 255; }),
            "startup UI contains visible progress graphics");
    // A top-down, 32-bit BI_RGB BMP stores readback BGRA without conversion.
    if (!path.empty()) {
        std::ofstream output(path, std::ios::binary);
        output.write("BM", 2); word(output, uint32_t(54 + pixels.size())); word(output, 0); word(output, 54);
        word(output, 40); word(output, 640); word(output, uint32_t(-360));
        word(output, 1 | (32u << 16)); word(output, 0); word(output, uint32_t(pixels.size()));
        word(output, 0); word(output, 0); word(output, 0); word(output, 0);
        output.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
        output.close();
        require(bool(output), "startup UI screenshot is written");
    }
    presentation.finish_preparation();
}
void run() {
    const auto directory = fs::temp_directory_path() / ("sfr-pipeline-prewarm-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{directory};
    environment("SFR_RENDER_SCALE", "100");
    environment("SFR_FULLSCREEN", "0");
    environment("SFR_GPU_PIPELINE", "1");
    environment("SFR_PIPELINE_CACHE_PATH", (directory / "driver.cache").string());
    environment("SFR_PIPELINE_PREWARM", "1");
    environment("SFR_SHADER_PACK", (directory / "fixture.pack").string());
    environment("SFR_SHADER_TRANSLATOR", (directory / "must-not-run-translator").string());
    environment("SFR_RUNTIME_SHADER_CACHE", (directory / "must-not-translate").string());
    sfr::NativeGraphics graphics;
    graphics.initialize();
    const bool vulkan = graphics.backend() == sfr::GraphicsBackend::vulkan;
    const uint32_t backend = uint32_t(graphics.backend());
    const auto vertex_code = compile(directory, vulkan, true);
    const auto pixel_code = compile(directory, vulkan, false);
    sfr::PipelineRecipe known;
    known.vertex = sfr::pipeline_shader_id(vertex_source);
    known.pixel = sfr::pipeline_shader_id(pixel_source);
    auto unknown = known;
    unknown.vertex.hash ^= 0x100;
    const std::array recipes{known, unknown};
    const auto shipped = directory / "shipped.manifest";
    require(sfr::save_pipeline_manifest_file(shipped, recipes, backend), "shipped recipes saved");
    environment("SFR_PIPELINE_MANIFEST", shipped.string());
    environment("SFR_PIPELINE_MANIFEST_LOCAL", (directory / "corrupt-pack-local.manifest").string());

    // A malformed required pack must still fail, unlike optional manifests.
    // This is the first lazy pack access: a throwing static initializer may
    // then retry against the repaired fixture without adding a reset API.
    std::ofstream(directory / "fixture.pack", std::ios::binary) << "broken pack";
    {
        sfr::NativePresentation presentation(graphics, 16, 16);
        sfr::NativeRenderer renderer(graphics, presentation);
        bool rejected = false;
        try { renderer.prepare_pipelines(); }
        catch (const sfr::RuntimeStop& stop) {
            require(stop.category == "shader-pack", "corrupt pack reports shader-pack category");
            rejected = true;
        }
        require(rejected, "prewarm must not silently accept a corrupt required shader pack");
    }
    write_pack(directory / "fixture.pack", vulkan, vertex_code, pixel_code);
    const auto* vertex_entry = sfr::packed_pipeline_shader(sfr::ShaderStage::vertex, known.vertex.hash, known.vertex.size);
    const auto* pixel_entry = sfr::packed_pipeline_shader(sfr::ShaderStage::pixel, known.pixel.hash, known.pixel.size);
    require(vertex_entry && pixel_entry, "current ABI pack resolves both fixture identities");
    require(!sfr::packed_pipeline_shader(sfr::ShaderStage::vertex, unknown.vertex.hash, unknown.vertex.size),
            "unknown source identity is skipped");
    std::vector<uint8_t> prepared_pixels;
    for (const bool enabled : {true, false}) {
        environment("SFR_PIPELINE_PREWARM", enabled ? "1" : "0");
        const auto learned = directory / (enabled ? "warm-local.manifest" : "cold-local.manifest");
        environment("SFR_PIPELINE_MANIFEST_LOCAL", learned.string());
        require(!fs::exists(learned), "each run starts with an absent local manifest");
        {
            sfr::NativePresentation presentation(graphics, 16, 16);
            // Outlive the renderer; these objects are independently created
            // after preparation, never the renderer's prepared shader pointers.
            std::unique_ptr<plume::RenderShader> vertex, pixel;
            sfr::NativeRenderer renderer(graphics, presentation);
            renderer.prepare_pipelines();
            require(renderer.take_pipeline_work().created == 0, "startup work is excluded from gameplay counts");
            vertex = live_shader(graphics, *vertex_entry);
            pixel = live_shader(graphics, *pixel_entry);
            sfr::NativeDraw draw;
            draw.vertex_count = 3;
            draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
            draw.vertex_entry = vertex_entry; draw.pixel_entry = pixel_entry;
            clear(presentation);
            renderer.draw(draw);
            require(renderer.take_pipeline_work().created == (enabled ? 0u : 1u),
                    "first gameplay draw reuses prewarm across independent shader objects only when enabled");
            const auto pixels = presentation.readback_color();
            expect_color(pixels, false);
            if (enabled) prepared_pixels = pixels;
            else require(pixels == prepared_pixels, "prewarm enabled and disabled produce identical pixels");
            renderer.draw(draw);
            require(renderer.take_pipeline_work().created == 0, "second identical draw creates no pipeline");
            // Red and alpha only; the cleared green/blue channels must remain
            // black, detecting accidental reuse of the full-write pipeline.
            draw.write_mask = 0x9;
            clear(presentation);
            renderer.draw(draw);
            require(renderer.take_pipeline_work().created == 1, "write-mask change creates exactly one new pipeline");
            expect_color(presentation.readback_color(), true);
        }
        const auto saved = sfr::load_pipeline_manifest_file(learned, backend);
        auto changed = known;
        changed.state.write_mask = 0x9;
        require(std::any_of(saved.begin(), saved.end(), [&](const auto& recipe) {
            return sfr::pipeline_recipe_key(recipe) == sfr::pipeline_recipe_key(changed);
        }), "renderer destruction persists the newly learned state recipe");
    }
    environment("SFR_PIPELINE_PREWARM", "1");
    // A previous pack can leave the entire local manifest full of obsolete
    // identities. They must be removed before applying the recipe capacity,
    // otherwise the valid bundled recipe never gets prepared or retained.
    const auto stale_local = directory / "full-stale-local.manifest";
    {
        std::vector<sfr::PipelineRecipe> stale(sfr::pipeline_manifest_max_recipes, known);
        for (size_t i = 0; i < stale.size(); ++i) stale[i].vertex.hash ^= uint64_t(i + 1);
        require(sfr::save_pipeline_manifest_file(stale_local, stale, backend), "full stale local manifest saved");
        require(sfr::load_pipeline_manifest_file(stale_local, backend).size() == sfr::pipeline_manifest_max_recipes,
                "stale fixture fills every local recipe slot with a distinct identity");
    }
    environment("SFR_PIPELINE_MANIFEST", shipped.string());
    environment("SFR_PIPELINE_MANIFEST_LOCAL", stale_local.string());
    {
        sfr::NativePresentation presentation(graphics, 16, 16);
        std::unique_ptr<plume::RenderShader> vertex, pixel;
        sfr::NativeRenderer renderer(graphics, presentation);
        renderer.prepare_pipelines();
        require(renderer.take_pipeline_work().created == 0, "stale manifest cleanup creates no gameplay pipeline");
        vertex = live_shader(graphics, *vertex_entry);
        pixel = live_shader(graphics, *pixel_entry);
        sfr::NativeDraw draw;
        draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        draw.vertex_entry = vertex_entry; draw.pixel_entry = pixel_entry;
        clear(presentation);
        renderer.draw(draw);
        require(renderer.take_pipeline_work().created == 0,
                "4096 stale local recipes cannot crowd out the valid bundled prewarm recipe");
        expect_color(presentation.readback_color(), false);
    }
    const auto refreshed = sfr::load_pipeline_manifest_file(stale_local, backend);
    require(refreshed.size() == 1 && sfr::pipeline_recipe_key(refreshed.front()) == sfr::pipeline_recipe_key(known),
            "renderer destruction replaces the full stale local manifest with the valid bundled recipe only");
    // Both missing and malformed optional manifests leave on-demand rendering
    // available. Use separate missing local files so learned data cannot hide it.
    for (const bool corrupt : {false, true}) {
        const auto manifest = directory / (corrupt ? "corrupt.manifest" : "missing.manifest");
        if (corrupt) std::ofstream(manifest, std::ios::binary) << "not a pipeline manifest";
        environment("SFR_PIPELINE_MANIFEST", manifest.string());
        environment("SFR_PIPELINE_MANIFEST_LOCAL", (directory / (corrupt ? "corrupt-local.manifest" : "missing-local.manifest")).string());
        sfr::NativePresentation presentation(graphics, 16, 16);
        const auto vertex = live_shader(graphics, *vertex_entry), pixel = live_shader(graphics, *pixel_entry);
        sfr::NativeRenderer renderer(graphics, presentation);
        renderer.prepare_pipelines();
        sfr::NativeDraw draw;
        draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        draw.vertex_entry = vertex_entry; draw.pixel_entry = pixel_entry;
        clear(presentation);
        renderer.draw(draw);
        require(renderer.take_pipeline_work().created == 1, "optional manifest failure retains on-demand pipeline creation");
        expect_color(presentation.readback_color(), false);
    }
    require(!fs::exists(directory / "must-not-translate"), "prewarm never invokes runtime translation for unknown identities");
    const char* screenshot_path = std::getenv("SFR_PREWARM_TEST_SCREENSHOT");
    screenshot(graphics, screenshot_path && *screenshot_path ? fs::u8path(screenshot_path) : fs::path{});
#ifdef _WIN32
    environment("SFR_PIPELINE_MANIFEST", shipped.string());
    environment("SFR_PIPELINE_MANIFEST_LOCAL", (directory / "cancel-local.manifest").string());
    {
        sfr::NativePresentation presentation(graphics, 16, 16);
        sfr::NativeRenderer renderer(graphics, presentation);
        require(PostMessageW(static_cast<HWND>(presentation.window_handle()), WM_CLOSE, 0, 0) != 0,
                "close message is queued before preparation starts");
        bool cancelled = false;
        try { renderer.prepare_pipelines(); }
        catch (const sfr::RuntimeStop& stop) {
            require(stop.category == "window-closed", "startup cancellation reports window-closed");
            cancelled = true;
        }
        require(cancelled && presentation.close_requested(), "startup preparation responds to queued close");
        require(renderer.take_pipeline_work().created == 0, "cancelled startup creates no gameplay pipeline");
    }
#endif
}
}
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string_view backend = std::string_view(argv[1]) == "--backend" && argc > 2 ? argv[2] : argv[1];
            require(backend == "vulkan" || backend == "d3d12", "backend argument must be vulkan or d3d12");
            environment("SFR_GRAPHICS", std::string(backend));
        }
        run();
        std::cout << "Native pipeline prewarm GPU regression passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
