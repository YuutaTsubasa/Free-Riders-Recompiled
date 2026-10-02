#include "native_graphics.h"
#include "native_presentation.h"
#include "native_renderer.h"
#include "guest_memory.h"
#include "resolution_shader_source.h"
#include "vulkan_shader_source.h"
#include "plume_render_interface.h"
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void environment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
std::string quote(const fs::path& path) {
#ifdef _WIN32
    return "\"" + path.string() + "\"";
#else
    std::string result = "'";
    for (char c : path.string()) result += c == '\'' ? std::string("'\\''") : std::string(1,c);
    return result + "'";
#endif
}
std::unique_ptr<plume::RenderShader> compile(sfr::NativeGraphics& graphics, const fs::path& directory,
                                          const std::string& header, const std::string& body, bool vertex) {
    const bool vulkan = graphics.backend() == sfr::GraphicsBackend::vulkan;
    const auto source = directory / (vertex ? "vertex.hlsl" : "pixel.hlsl");
    const auto output = directory / (vertex ? "vertex.bin" : "pixel.bin");
    std::string text = header + body;
    if (vulkan) {
        auto converted = sfr::vulkan_shader_source(text);
        require(converted.has_value(), "resolution fixture converts to Vulkan");
        text = *converted;
    }
    std::ofstream(source, std::ios::binary) << text;
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
    auto shader = graphics.device().createShader(code.data(), code.size(), "shaderMain",
        vulkan ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL);
    require(bool(shader), "resolution fixture shader creation succeeds");
    return shader;
}
void batched_texture_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
float4 shaderMain(uint id : SV_VertexID) : SV_Position {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 return float4(p[id],0.5,1);
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain() : SV_Target { return tfetch2D(textures0,samplers0,float2(0.5,0.5),float2(0,0)); }
)", false);
    environment("SFR_RENDER_SCALE", "100");
    for (bool batch : {false, true}) {
        environment("SFR_TEXTURE_UPLOAD_BATCH", batch ? "1" : "0");
        sfr::GuestMemory memory;
        constexpr uint32_t physical = 0x100000, address = 0xA0100000;
        memory.map(address, 0x10000);
        sfr::NativePresentation presentation(graphics, 16, 16);
        sfr::NativeRenderer renderer(graphics, presentation);
        sfr::FetchWords fetch{2u | (1u << 22), physical | 6u | (2u << 6), 0u, 0x688u << 1, 0u, 1u << 9};
        sfr::NativeDraw draw;
        draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        draw.shared.sampler[0] = renderer.sampler({});
        for (unsigned frame=0; frame<5; ++frame) {
            sfr::NativeClear clear{}; clear.color = true; presentation.clear(clear);
            for (unsigned column=0; column<16; ++column) {
                const uint32_t red = 8 + column*15, green = 20 + frame*40;
                memory.store<uint32_t>(address, 0xFF000000u | (green << 8) | red);
                renderer.invalidate(physical, 128);
                draw.shared.texture_2d[0] = renderer.texture(memory, fetch);
                presentation.set_raster_state({0,0,16,16}, {int(column),0,int(column+1),16});
                renderer.draw(draw);
            }
            auto before = renderer.take_pipeline_work();
            require(before.upload_submissions == 0, "texture copies accumulate without per-texture submissions");
            if (frame == 0 || frame == 4) {
                const auto pixels = presentation.readback_color();
                for (unsigned y=0; y<16; ++y) for (unsigned x=0; x<16; ++x) {
                    const auto at = (y*16+x)*4;
                    require(pixels[at+2] == 8+x*15 && pixels[at+1] == 20+frame*40 && pixels[at+3] == 255,
                        "batched textures retain each guest version and survive asynchronous slot reuse");
                }
            } else {
                presentation.present();
            }
            const auto after = renderer.take_pipeline_work();
            require(after.upload_submissions == (batch ? 1u : 0u), "one copy submission per graphics batch");
        }
        // An upload with no draw must still be drained before owner teardown.
        renderer.invalidate(physical, 128);
        renderer.texture(memory, fetch);
        presentation.flush();
        require(renderer.take_pipeline_work().upload_submissions == (batch ? 1u : 0u),
            "empty graphics flush drains uploads");
    }
    environment("SFR_TEXTURE_UPLOAD_BATCH", "1");
}

void resolved_allocation_reuse_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
float4 shaderMain(uint id : SV_VertexID) : SV_Position {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 return float4(p[id],0.5,1);
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain() : SV_Target {
 float4 color = tfetch2D(textures0,samplers0,float2(0.5,0.5),float2(0,0));
 return float4(color.rg, getPixelCoord(textures0,float2(0.5,0.5)).x / 16.0, 1);
})", false);
    for (uint32_t percent : {50u, 200u}) for (bool submitted : {false, true}) {
        environment("SFR_RENDER_SCALE", std::to_string(percent));
        sfr::GuestMemory memory;
        constexpr uint32_t physical = 0x101000, other = 0x104000;
        memory.map(0xA0100000, 0x10000);
        const auto fetch = [](uint32_t base) {
            return sfr::FetchWords{2u | (1u << 22), base | 6u | (2u << 6), 0u, 0x688u << 1, 0u, 1u << 9};
        };
        unsigned asynchronous_flushes = 0;
        sfr::NativePresentation presentation(graphics, 16, 16);
        sfr::NativeRenderer renderer(graphics, presentation);
        sfr::NativeClear clear{};
        clear.color = true; clear.color_value = {1,0,0,1};
        presentation.clear(clear);
        const uint32_t old = renderer.adopt_resolved_target(physical);
        const uint32_t unrelated = renderer.adopt_resolved_target(other);
        clear.color_value = {0,0,0,0}; presentation.clear(clear);
        sfr::NativeDraw draw;
        draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        draw.shared.texture_2d[0] = old;
        draw.shared.sampler[0] = renderer.sampler({});
        presentation.set_raster_state({0,0,16,16}, {0,0,8,16});
        renderer.draw(draw);
        // Exercise both a recorded consumer and a previously submitted one.
        presentation.after_flush([&](bool complete) { if (!complete) ++asynchronous_flushes; });
        if (submitted) presentation.present();
        const uint64_t generation = renderer.texture_generation();
        // The resolve base is inside, rather than at the start of, this freed
        // allocation. Its replacement has the same address and new CPU bytes.
        renderer.invalidate(physical - 0x1000, 0x2000);
        const uint64_t invalidated_generation = renderer.texture_generation();
        renderer.invalidate(physical - 0x1000, 0x2000);
        require(renderer.texture_generation() == invalidated_generation, "repeated retirement does not publish another change");
        memory.store<uint32_t>(0xA0000000u | physical, 0xFF00FF00);
        const uint32_t replacement = renderer.texture(memory, fetch(physical));
        draw.shared.texture_2d[0] = replacement;
        presentation.set_raster_state({0,0,16,16}, {8,0,16,16});
        renderer.draw(draw);
        const auto pixels = presentation.readback_color();
        const uint32_t extent = 16 * percent / 100;
        for (uint32_t y=0; y<extent; ++y) for (uint32_t x=0; x<extent; ++x) {
            const size_t at = (y*extent+x)*4;
            const bool original = x < extent/2;
            require(pixels[at+2] == (original ? 255 : 0) && pixels[at+1] == (original ? 0 : 255),
                    "freed resolve yields new guest pixels while recorded consumers retain old pixels");
            require(std::abs(int(pixels[at]) - (original ? 128 : 8)) <= 1 && pixels[at+3] == 255,
                    "replacement texture uses its own dimensions at non-default render scale");
        }
        require(invalidated_generation != generation, "resolve retirement invalidates memoized guest bindings");
        require(replacement != old, "resolve descriptor stays unavailable until consuming GPU work completes");
        require(!submitted || asynchronous_flushes != 0, "submitted case exercises an asynchronous frame");
        require(renderer.texture(memory, fetch(other)) == unrelated, "invalidation preserves an unrelated resolve");
        // Completion can now recycle the old descriptor as an ordinary texture.
        // The dimension channel detects a resolved-index bit left on that slot.
        memory.store<uint32_t>(0xA0106000, 0xFF00FF00);
        const uint32_t recycled = renderer.texture(memory, fetch(0x106000));
        require(recycled == old, "completed resolve descriptor is reclaimed");
        memory.store<uint32_t>(0xA0107000, 0xFF0000FF);
        require(renderer.texture(memory, fetch(0x107000)) != recycled, "repeated invalidation never recycles one slot twice");
        draw.shared.texture_2d[0] = recycled;
        presentation.set_raster_state({0,0,16,16}, {0,0,16,16});
        renderer.draw(draw);
        const auto reused = presentation.readback_color();
        for (size_t at=0; at<reused.size(); at+=4)
            require(reused[at+2] == 0 && reused[at+1] == 255 && std::abs(int(reused[at])-8)<=1,
                    "recycled descriptor has fresh pixels and no resolved dimension tag");
        std::cerr << "RESOLVE_LIFETIME scale=" << percent << " submitted=" << submitted << " passed\n";
    }
}

void constant_upload_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
#ifndef __spirv__
cbuffer VSConstants : register(b0, space4) { float4 vsConstants[256]; };
#endif
struct Output { float4 position : SV_Position; float red : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position = float4(p[id],0.5,1);
#ifdef __spirv__
 o.red = asfloat(vk::RawBufferLoad<uint>(g_PushConstants.VertexShaderConstants + 4092));
#else
 o.red = vsConstants[255].w;
#endif
 return o;
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
#ifndef __spirv__
cbuffer PSConstants : register(b1, space4) { float4 psConstants[256]; };
#endif
// Retain the VS signature order for D3D12 linkage even though position is unused.
float4 shaderMain(float4 position : SV_Position, float red : TEXCOORD0) : SV_Target {
#ifdef __spirv__
 float green = asfloat(vk::RawBufferLoad<uint>(g_PushConstants.PixelShaderConstants + 0));
#else
 float green = psConstants[0].x;
#endif
 return float4(red,green,0.25,1);
})", false);
    environment("SFR_RENDER_SCALE", "100");
    for (bool reuse : {false, true}) {
        std::cerr << "CONSTANT_READBACK reuse=" << reuse << " begin\n";
        environment("SFR_CONSTANT_UPLOAD_REUSE", reuse ? "1" : "0");
        unsigned asynchronous_flushes = 0;
        sfr::NativePresentation presentation(graphics, 16, 16);
        sfr::NativeRenderer renderer(graphics, presentation);
        presentation.after_flush([&](bool complete) { if (!complete) ++asynchronous_flushes; });
        sfr::NativeClear clear{};
        clear.color = true;
        presentation.clear(clear);
        sfr::NativeDraw draw;
        draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        const std::array<float,4> red{0.25f,0.25f,0.75f,0.75f}, green{0.5f,0.5f,0.5f,0.875f};
        for (int column = 0; column < 4; ++column) {
            draw.vertex_constants.back() = std::bit_cast<uint32_t>(red[column]);
            draw.pixel_constants.front() = std::bit_cast<uint32_t>(green[column]);
            presentation.set_raster_state({0,0,16,16}, {column*4,0,(column+1)*4,16});
            renderer.draw(draw);
        }
        require(renderer.take_pipeline_work().constant_saved_bytes == (reuse ? 16384 : 0),
                "identical VS/PS stages skip writes independently, only when enabled");
        std::cerr << "CONSTANT_READBACK columns recorded\n";
        const auto pixels = presentation.readback_color();
        std::cerr << "CONSTANT_READBACK columns read\n";
        for (uint32_t y=0; y<16; ++y) for (uint32_t x=0; x<16; ++x) {
            const size_t at = (y*16+x)*4;
            require(std::abs(int(pixels[at+2])-int(red[x/4]*255+0.5f))<=1 &&
                    std::abs(int(pixels[at+1])-int(green[x/4]*255+0.5f))<=1 &&
                    std::abs(int(pixels[at])-64)<=1 && pixels[at+3]==255,
                    "GPU sees each draw's exact VS and PS constants at its own offset");
        }
        // readback flushes and recycles the ring. Identical values must upload
        // again; merely finding old bytes still in mapped storage is not enough.
        presentation.set_raster_state({0,0,16,16}, {0,0,16,16});
        renderer.draw(draw);
        require(renderer.take_pipeline_work().constant_saved_bytes == 0, "flush invalidates both stage offsets");
        renderer.draw(draw);
        require(renderer.take_pipeline_work().constant_saved_bytes == (reuse ? 8192 : 0), "reuse resumes after the fresh upload");
        const auto repeated = presentation.readback_color();
        std::cerr << "CONSTANT_READBACK recycled ring read\n";
        for (size_t at=0; at<repeated.size(); at+=4)
            require(std::abs(int(repeated[at+2])-191)<=1 && std::abs(int(repeated[at+1])-223)<=1,
                    "recycled ring produces fresh constants rather than stale addresses");
        // Rotate through both rings while previous GPU submissions can remain
        // in flight. Every fresh frame must upload before it can reuse again.
        for (int frame=0; frame<4; ++frame) {
            renderer.draw(draw);
            require(renderer.take_pipeline_work().constant_saved_bytes == 0, "asynchronous ring switch invalidates both stages");
            renderer.draw(draw);
            require(renderer.take_pipeline_work().constant_saved_bytes == (reuse ? 8192 : 0), "new ring reuses only its own upload");
            presentation.present();
        }
        require(asynchronous_flushes >= 4, "test exercised asynchronous present callbacks");
        renderer.draw(draw);
        renderer.draw(draw);
        (void)renderer.take_pipeline_work();
        // A near-capacity reservation forces the 64 MiB ring to flush after
        // those draws. No huge vertex copy is needed to exercise this path.
        require(!renderer.vertex_space((64ull << 20) - 32768, 0).empty(), "near-capacity reservation fits one ring");
        require(renderer.take_pipeline_work().ring_flushes == 1, "reservation exercises the capacity flush path");
        renderer.draw(draw);
        require(renderer.take_pipeline_work().constant_saved_bytes == 0, "capacity flush invalidates cached offsets");
        const auto final = presentation.readback_color();
        for (size_t at=0; at<final.size(); at+=4)
            require(std::abs(int(final[at+2])-191)<=1 && std::abs(int(final[at+1])-223)<=1,
                    "ring switches and capacity flush preserve rendered constants");
    }
    environment("SFR_CONSTANT_UPLOAD_REUSE", "0");
}
// Preserve depth/stencil across color-only work before attempting to remove
// unused attachments from those draws. This is also an oracle for alternating
// target modes: a color-only draw may still carry an enabled depth-write bit.
void depth_stencil_preservation_readback(sfr::NativeGraphics& graphics, const fs::path& directory,
                                       const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
#ifndef __spirv__
cbuffer VSConstants : register(b0, space4) { float4 vsConstants[256]; };
#endif
float4 shaderMain(uint id : SV_VertexID) : SV_Position {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
#ifdef __spirv__
 float z = asfloat(vk::RawBufferLoad<uint>(g_PushConstants.VertexShaderConstants + 0));
#else
 float z = vsConstants[0].x;
#endif
 return float4(p[id],z,1);
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
#ifndef __spirv__
cbuffer PSConstants : register(b1, space4) { float4 psConstants[256]; };
#endif
float4 shaderMain() : SV_Target {
#ifdef __spirv__
 return asfloat(vk::RawBufferLoad<uint4>(g_PushConstants.PixelShaderConstants + 0));
#else
 return psConstants[0];
#endif
})", false);
    for (uint32_t percent : {50u, 100u, 200u}) {
        environment("SFR_RENDER_SCALE", std::to_string(percent));
        sfr::NativePresentation presentation(graphics, 16, 16);
        sfr::NativeRenderer renderer(graphics, presentation);
        sfr::NativeDraw draw;
        draw.vertex_count = 3; draw.stride = 4;
        const std::array<uint8_t, 12> vertices{};
        draw.vertices = vertices; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        const auto paint = [&](float z, std::array<float, 4> color) {
            draw.vertex_constants[0] = std::bit_cast<uint32_t>(z);
            for (size_t i=0; i<color.size(); ++i)
                draw.pixel_constants[i] = std::bit_cast<uint32_t>(color[i]);
            renderer.draw(draw);
        };
        const auto check = [&](std::array<uint8_t, 4> left, std::array<uint8_t, 4> right) {
            const auto pixels = presentation.readback_color();
            const auto extent = presentation.render_width();
            for (uint32_t y=0; y<extent; ++y) for (uint32_t x=0; x<extent; ++x) {
                const auto& expected = x < extent/2 ? left : right;
                for (size_t channel=0; channel<4; ++channel)
                    require(std::abs(int(pixels[(y*extent+x)*4+channel])-int(expected[channel]))<=1,
                            "depth/stencil contents survive color-only draws and attachment transitions");
            }
        };
        for (unsigned cycle=0; cycle<3; ++cycle) {
            sfr::NativeClear clear{};
            clear.color = clear.depth = clear.stencil = true;
            clear.color_value = {0,0,0,1};
            presentation.clear(clear);
            presentation.set_raster_state({0,0,16,16}, {0,0,8,16});
            draw.depth_enabled = draw.depth_write = draw.stencil_enabled = true;
            draw.stencil_reference = 1;
            draw.stencil_front.compareFunction = draw.stencil_back.compareFunction = plume::RenderComparisonFunction::ALWAYS;
            draw.stencil_front.passOp = draw.stencil_back.passOp = plume::RenderStencilOp::REPLACE;
            paint(0.25f, {1,0,0,1});
            presentation.set_raster_state({0,0,16,16}, {0,0,16,16});
            draw.depth_enabled = draw.stencil_enabled = false;
            // Deliberately keep depth_write true: without a depth test this
            // color-only draw must not replace the stored depth with zero.
            draw.blend.blendEnabled = true;
            draw.blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
            draw.blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
            paint(0.0f, {0,0,1,0.5f});
            draw.blend.blendEnabled = false;
            draw.depth_enabled = true; draw.depth_write = false;
            paint(0.75f, {0,1,0,1});
            check({128,0,128,128}, {0,255,0,255});
            draw.depth_enabled = false; draw.stencil_enabled = true;
            draw.stencil_front.compareFunction = draw.stencil_back.compareFunction = plume::RenderComparisonFunction::EQUAL;
            draw.stencil_front.passOp = draw.stencil_back.passOp = plume::RenderStencilOp::KEEP;
            paint(0.0f, {1,1,0,1});
            check({0,255,255,255}, {0,255,0,255});
            draw.stencil_enabled = false;
            paint(0.0f, {0,0,1,1});
            clear.color = clear.stencil = false; clear.depth = true;
            presentation.clear(clear);
            draw.depth_enabled = true;
            paint(0.75f, {1,1,1,1});
            check({255,255,255,255}, {255,255,255,255});
            draw.depth_enabled = false; draw.stencil_enabled = true;
            paint(0.0f, {1,0,0,1});
            check({0,0,255,255}, {255,255,255,255});
            draw.stencil_enabled = false;
        }
        std::cerr << "DEPTH_STENCIL_PRESERVATION scale=" << percent << " cycles=3 passed\n";
    }
}
void run() {
    environment("SFR_GPU_PIPELINE", "1");
    const fs::path directory = fs::temp_directory_path() / ("sfr-resolution-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{directory};
    auto header = sfr::resolution_shader_header(read(fs::path(SFR_SOURCE_DIR) / "tools/XenosRecomp/XenosRecomp/shader_common.h"));
    // Mirror the production extended header's shared and push-constant ABI.
    const std::string push = "    uint64_t SharedConstants;";
    header.replace(header.find(push), push.size(), push + "\n    uint64_t VertexPalette;\n    uint64_t LoopConstants;");
    header += R"(
#ifndef __spirv__
cbuffer Shared : register(b2, space4) {
 uint4 textures[4] : packoffset(c0); uint4 arrays[4] : packoffset(c4);
 uint4 cubes[4] : packoffset(c8); uint4 samplers[4] : packoffset(c12);
 DEFINE_SHARED_CONSTANTS();
 float2 g_ScreenSpaceScale : packoffset(c20.x);
 float2 g_ResolvedTextureScale : packoffset(c20.z);
};
uint g_SpecConstants() { return 0; }
#else
// Match the pinned translator's explicit byte-offset form, including zero.
#define textures0 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)
#define samplers0 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 192)
#endif
#ifndef __spirv__
#define textures0 textures[0].x
#define samplers0 samplers[0].x
#endif
)";
    sfr::NativeGraphics graphics;
    graphics.initialize();
    depth_stencil_preservation_readback(graphics, directory, header);
    resolved_allocation_reuse_readback(graphics, directory, header);
    batched_texture_readback(graphics, directory, header);
    const auto vertex = compile(graphics, directory, header, R"(
float4 shaderMain(uint id : SV_VertexID) : SV_Position {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 return float4(p[id], 0.5, 1);
})", true);
    // The actual translated vertex epilogue: guest screen coordinates followed
    // by the Xenos half-pixel correction. A -0.5 guest position must remain an
    // exact logical edge even when a logical pixel spans two host pixels.
    const auto screen_vertex = compile(graphics, directory, header, R"(
struct Vertex { float4 oPos : SV_Position; };
Vertex shaderMain(uint id : SV_VertexID) {
 float2 p[3] = {float2(3.5,-0.5), float2(3.5,31.5), float2(35.5,-0.5)};
 Vertex output; output.oPos = float4(p[id],0.5,1);
 if (any(g_ScreenSpaceScale != 0.0)) output.oPos.xy = output.oPos.xy * g_ScreenSpaceScale + float2(-1.0, 1.0) * output.oPos.w;
 output.oPos.xy += g_HalfPixelOffset * output.oPos.w;
 return output;
})", true);
    // All helpers are exercised on a real resolved texture. Encode independent
    // results in channels so a wrong physical texel dimension is observable.
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain() : SV_Target {
 float r = tfetch2D(textures0, samplers0, float2(1.5/16.0,0.5), float2(4,0)).r;
 float g = getWeights2D(textures0, samplers0, float2(4.25/16.0,0.5), float2(0,0)).x;
 float b = getPixelCoord(textures0, float2(4.25/16.0,0.5)).x / 16.0;
 float a = tfetch2DBicubic(textures0, samplers0, float2(0.5,0.5), float2(0,0)).a;
 return float4(r,g,b,a);
})", false);
    for (uint32_t percent : {50u, 75u, 100u, 150u, 200u}) {
        environment("SFR_RENDER_SCALE", std::to_string(percent));
        sfr::NativePresentation presentation(graphics, 16, 16);
        require(presentation.width() == 16 && presentation.height() == 16, "guest dimensions remain logical");
        sfr::NativeRenderer renderer(graphics, presentation);
        sfr::NativeClear clear{};
        clear.color = true;
        clear.color_value = {0, 0, 0, 1};
        presentation.clear(clear);
        for (int x = 0; x < 16; x += 4) {
            clear.color_value[0] = float(x) / 16;
            const plume::RenderRect rect(x, 0, x + 4, 16);
            presentation.clear(clear, std::span(&rect, 1));
        }
        const uint32_t resolved = renderer.adopt_resolved_target(0x1000);
        clear.color_value = {0,0,0,0};
        presentation.clear(clear);
        sfr::NativeDraw draw;
        draw.vertex_count = 3;
        draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        draw.shared.texture_2d[0] = resolved;
        draw.shared.sampler[0] = renderer.sampler({});
        renderer.draw(draw);
        const auto pixels = presentation.readback_color();
        const uint32_t extent = 16 * percent / 100;
        require(pixels.size() == extent * extent * 4, "resolved draw uses the actual scaled texture");
        for (size_t at = 0; at < pixels.size(); at += 4) {
            require(std::abs(int(pixels[at]) - 68) <= 1, "getPixelCoord preserves guest dimensions");
            require(std::abs(int(pixels[at + 1]) - 191) <= 1, "getWeights2D preserves guest dimensions");
            require(std::abs(int(pixels[at + 2]) - 64) <= 1, "tfetch2D offsets stay in guest pixels");
            require(pixels[at + 3] == 255, "bicubic helper strips resolution metadata before indexing");
        }
        presentation.clear(clear);
        presentation.set_raster_state({0,0,8,16}, {4,0,8,16});
        renderer.draw(draw);
        presentation.set_raster_state({8,0,8,16}, {8,0,12,16});
        renderer.draw(draw);
        const auto split = presentation.readback_color();
        for (uint32_t y=0; y<extent; ++y) for (uint32_t x=0; x<extent; ++x) {
            const auto alpha = split[(y * extent + x) * 4 + 3];
            require(alpha == (x >= extent/4 && x < extent*3/4 ? 255 : 0),
                    "two scaled viewports respect adjacent player scissors without overwriting one another");
        }
        presentation.set_raster_state({0,0,16,16}, {0,0,16,16});
        draw.shared.texture_2d[0] = renderer.placeholder_texture();
        renderer.draw(draw);
        const auto ordinary = presentation.readback_color();
        for (size_t at=0; at<ordinary.size(); at+=4)
            require(std::abs(int(ordinary[at])-4)<=1 && std::abs(int(ordinary[at+1])-195)<=1 &&
                    ordinary[at+2]==255 && ordinary[at+3]==255,
                    "ordinary textures retain their actual dimensions and untagged descriptor indices");
        presentation.clear(clear);
        draw.vertex_shader = screen_vertex.get();
        draw.shared.screen_space_scale[0] = 2.0f / 16;
        draw.shared.screen_space_scale[1] = -2.0f / 16;
        draw.shared.half_pixel_offset[0] = 1.0f / 16;
        draw.shared.half_pixel_offset[1] = -1.0f / 16;
        renderer.draw(draw);
        const auto screen = presentation.readback_color();
        for (uint32_t y=0; y<extent; ++y) for (uint32_t x=0; x<extent; ++x)
            require(screen[(y * extent + x) * 4 + 3] == (x>=extent/4 ? 255 : 0),
                    "translated screen-space and half-pixel epilogue preserve logical geometry edges");
    }
    constant_upload_readback(graphics, directory, header);
}
}
int main() {
    try { run(); std::cout << "Internal resolution readback checks passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
