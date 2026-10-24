#include "native_graphics.h"
#include "native_presentation.h"
#include "native_renderer.h"
#include "guest_memory.h"
#include "resolution_shader_source.h"
#include "vulkan_shader_source.h"
#include "plume_render_interface.h"
#include "native_formats.h"
#include "native_vertex_layout.h"
#include "shader_inputs.h"
#include "shader_cache.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string_view>
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
std::string compile_bytecode(sfr::NativeGraphics& graphics, const fs::path& directory,
                             const std::string& header, const std::string& body, bool vertex, bool library=false) {
    const bool vulkan = graphics.backend() == sfr::GraphicsBackend::vulkan;
    require(!library || (vertex && !vulkan), "raw library compiles only on D3D12");
    const auto source = directory / (library ? "vertex.raw.hlsl" : vertex ? "vertex.hlsl" : "pixel.hlsl");
    const auto output = directory / (library ? "vertex.raw.bin" : vertex ? "vertex.bin" : "pixel.bin");
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
    const std::string command = quote(dxc) + " -T " + (library ? "lib_6_3" : vertex ? "vs_6_0" : "ps_6_0") +
        (library ? "" : " -E shaderMain") + " -HV 2021 -all-resources-bound -Qstrip_debug" +
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
    return read(output);
}
std::unique_ptr<plume::RenderShader> compile(sfr::NativeGraphics& graphics,const fs::path& directory,
                                           const std::string& header,const std::string& body,bool vertex) {
    const bool vulkan=graphics.backend()==sfr::GraphicsBackend::vulkan;
    const auto code=compile_bytecode(graphics,directory,header,body,vertex);
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
// The original upstream CPU oracle: same signed10 clamp, lround and SNORM16 IA input.
std::vector<uint8_t> dec3n_reference(std::span<const uint8_t> input,uint32_t stride,uint32_t count,
                                     std::span<const uint32_t> offsets,bool guest,size_t prefix) {
    std::array<int16_t,1024> table{};
    for(uint32_t bits=0;bits<1024;++bits){const int32_t value=int32_t(bits<<22)>>22;
        table[bits]=int16_t(std::lround((std::max)(float(value)/511.0f,-1.0f)*32767.0f));}
    std::vector<uint8_t> host(size_t(count)*stride);
    const auto source=input.subspan(prefix,host.size());
    if(guest)sfr::swap_words_into(host,source);else std::memcpy(host.data(),source.data(),host.size());
    const uint32_t wide=stride+8*uint32_t(offsets.size());std::vector<uint8_t> result(size_t(count)*wide);
    for(uint32_t v=0;v<count;++v){const auto* from=host.data()+size_t(v)*stride;auto* to=result.data()+size_t(v)*wide;
        std::memcpy(to,from,stride);
        for(size_t f=0;f<offsets.size();++f){uint32_t word;std::memcpy(&word,from+offsets[f],4);
            const int16_t out[]{table[word&1023],table[(word>>10)&1023],table[(word>>20)&1023],32767};
            std::memcpy(to+stride+8*f,out,8);}}
    return result;
}
void gpu_dec3n_readback(sfr::NativeGraphics& graphics,const fs::path& directory,const std::string& header) {
    const bool raw=graphics.backend()==sfr::GraphicsBackend::d3d12;
    const char* requested=std::getenv("SFR_GRAPHICS");
    require(requested && std::string_view(requested)==(raw?"d3d12":"vulkan"),
            "DEC3N fixture requires the requested real backend without fallback");
    if(raw)require(std::getenv("SFR_D3D12_DRED") && std::string_view(std::getenv("SFR_D3D12_DRED"))=="1",
                   "D3D12 DEC3N fixture requires DRED");
    environment("SFR_RENDER_SCALE","100");
    constexpr uint32_t columns=64,rows=49,tile=4,width=columns*tile,height=rows*tile;
    constexpr std::array<const char*,9> names{"Normal0","Normal1","Normal2","Normal3","Tangent0","Tangent1","Tangent2","Tangent3","Binormal0"};
    constexpr std::array<const char*,9> references{"Position0","Position1","Position2","Position3","TexCoord0","TexCoord1","TexCoord2","TexCoord3","Color0"};
    constexpr std::array<const char*,9> ref_semantics{"POSITION","POSITION","POSITION","POSITION","TEXCOORD","TEXCOORD","TEXCOORD","TEXCOORD","COLOR"};
    constexpr std::array<uint32_t,9> ref_indices{0,1,2,3,0,1,2,3,0},ref_locations{0,1,2,3,13,14,15,16,17};
    std::ostringstream source;
    source<<"struct VertexShaderInput\n{\n";
    const auto member=[&](const std::string& name,const char* semantic,uint32_t index,uint32_t location){
        if(!raw)source<<"[[vk::location("<<location<<")]] ";
        source<<"float4 i"<<name<<" : "<<semantic<<index<<";\n";
    };
    for(uint32_t bit=0;bit<9;++bit)member(names[bit],bit<4?"NORMAL":bit<8?"TANGENT":"BINORMAL",bit<8?bit%4:0,4+bit);
    for(uint32_t bit=0;bit<9;++bit)member(references[bit],ref_semantics[bit],ref_indices[bit],ref_locations[bit]);
    member("BlendWeight0","BLENDWEIGHT",0,19);
    source<<"\n};\nstruct O{float4 position:SV_Position;nointerpolation float4 color:TEXCOORD0;};\n"
              "[shader(\"vertex\")]\nO shaderMain(VertexShaderInput input,uint id:SV_VertexID){uint4 bad=uint4(0,0,0,0);\n";
    for(uint32_t bit=0;bit<9;++bit)
        source<<"bad|=asuint(input.i"<<names[bit]<<")^asuint(input.i"<<references[bit]<<");\n";
    source<<"bool4 good=bad==0;float2 corners[6]={float2(0,0),float2(0,1),float2(1,0),float2(1,0),float2(0,1),float2(1,1)};"
              "uint cell=id/6;float2 p=float2(cell%64,cell/64)+corners[id%6];O o;"
              "o.position=float4(p.x*(2.0/64.0)-1.0,1.0-p.y*(2.0/49.0),0.5,1);"
              "if(input.iBlendWeight0.x<0){uint bits=asuint(input.iBlendWeight0.x==-1?input.iNormal0.x:input.iPosition0.x);"
              "o.color=float4(bits&255,(bits>>8)&255,(bits>>16)&255,bits>>24)/255.0;return o;}"
              "o.color=float4(good.x?0:1,all(good)?1:0,input.iBlendWeight0.x,good.w?1:0);return o;}";
    const auto legacy_source=source.str();
    const auto legacy_code=compile_bytecode(graphics,directory,header,legacy_source,true);
    const auto legacy_vertex=graphics.device().createShader(legacy_code.data(),legacy_code.size(),"shaderMain",
        raw?plume::RenderShaderFormat::DXIL:plume::RenderShaderFormat::SPIRV);
    require(bool(legacy_vertex),"legacy DEC3N oracle shader exists");
    std::string library;
    if(raw){const auto adapted=sfr::dec3n_vertex_shader_source(legacy_source);
        require(adapted.has_value(),"production DEC3N source adapter accepts the fixture's actual input structure");
        library=compile_bytecode(graphics,directory,header,*adapted,true,true);}
    const auto bytes=[](const std::string& text){return std::span(reinterpret_cast<const uint8_t*>(text.data()),text.size());};
    sfr::ShaderCacheEntry entry{bytes(legacy_source),bytes(legacy_code),sfr::ShaderStage::vertex,0};
    entry.dec3n_dxil=bytes(library);
    const auto pixel=compile(graphics,directory,header,
        "float4 shaderMain(float4 p:SV_Position,nointerpolation float4 color:TEXCOORD0):SV_Target{return color;}",false);
    // Shader bytecode/entry owners are declared before the renderer; none are replaced while a PSO can retain them.
    sfr::NativePresentation presentation(graphics,width,height);
    sfr::NativeRenderer renderer(graphics,presentation);
    sfr::NativeVertexLayoutCache cache;
    sfr::NativeClear clear{};clear.color=true;
    const auto write_word=[](std::vector<uint8_t>& out,size_t at,uint32_t value,bool guest){
        for(unsigned b=0;b<4;++b)out.at(at+b)=uint8_t(value>>(guest?24-8*b:8*b));
    };
    const auto element=[](auto& layout,uint32_t location)->plume::RenderInputElement&{
        const auto found=std::find_if(layout.begin(),layout.end(),[&](const auto& e){return e.location==location;});
        require(found!=layout.end(),"DEC3N reference input exists");return *found;
    };
    struct Probe {sfr::NativeDraw draw;std::vector<uint8_t> payload;uint32_t mask=0;};
    const auto make=[&](const std::vector<uint32_t>& words,uint32_t stride,const std::vector<std::pair<uint32_t,uint32_t>>& fields,
                        bool order=true,size_t prefix=0,uint8_t tag=0){
        require(!words.empty() && !fields.empty() && words.size()%fields.size()==0,"nonempty finite packed corpus");
        const uint32_t cells=uint32_t(words.size()/fields.size()),count=cells*6;
        std::vector<uint8_t> declaration(fields.size()*12),input(prefix+size_t(count)*stride+8,0x5a);
        std::vector<uint32_t> offsets;
        for(uint32_t f=0;f<fields.size();++f){const auto [bit,offset]=fields[f];require(bit<9,"packed semantic range");
            write_word(declaration,f*12,offset,true);write_word(declaration,f*12+4,0x2A2187,true);
            declaration[f*12+9]=uint8_t(bit<4?3:bit<8?6:7);declaration[f*12+10]=uint8_t(bit<8?bit%4:0);offsets.push_back(offset);
            for(uint32_t cell=0;cell<cells;++cell)for(uint32_t v=0;v<6;++v)
                write_word(input,prefix+size_t(cell*6+v)*stride+offset,words[cell*fields.size()+f],order==true);
        }
        const uint32_t wide=stride+uint32_t(fields.size())*8;
        const auto converted=dec3n_reference(input,stride,count,offsets,order,prefix);
        Probe result;const auto& layout=cache.get(0xdec30000,stride,declaration,raw);
        result.mask=layout.dec3n_mask;result.draw.stride=wide+4;result.draw.vertex_count=count;result.draw.elements=layout.elements;
        result.draw.vertex_shader=raw&&layout.dec3n_mask?renderer.specialized_vertex(entry,layout.dec3n_mask):legacy_vertex.get();
        result.draw.vertex_dec3n_mask=layout.dec3n_mask;result.draw.pixel_shader=pixel.get();
        result.draw.cull=plume::RenderCullMode::NONE;
        for(uint32_t f=0;f<fields.size();++f){const auto bit=fields[f].first;
            auto& ref=element(result.draw.elements,ref_locations[bit]);ref.format=plume::RenderFormat::R16G16B16A16_SNORM;
            ref.slotIndex=0;ref.alignedByteOffset=stride+8*f;
        }
        auto& color=element(result.draw.elements,19);
        require(color.semanticName && std::string_view(color.semanticName)=="BLENDWEIGHT" && color.semanticIndex==0,
                "numeric tag location19 is exactly BLENDWEIGHT0");
        color.format=plume::RenderFormat::R32_FLOAT;color.slotIndex=0;color.alignedByteOffset=wide;
        result.payload.resize(size_t(count)*result.draw.stride);
        const float color_value=float(tag)/255.0f;
        for(uint32_t v=0;v<count;++v){std::memcpy(result.payload.data()+size_t(v)*result.draw.stride,converted.data()+size_t(v)*wide,wide);
            std::memcpy(result.payload.data()+size_t(v)*result.draw.stride+wide,&color_value,4);}
        return result;
    };
    const auto paint=[&](Probe& probe,uint32_t x=0){probe.draw.vertices=probe.payload;
        presentation.set_raster_state({float(x),0,width,height},{int(x),0,width,height});renderer.draw(probe.draw);};
    const auto check=[&](uint32_t cells,const std::vector<uint8_t>& tags,const Probe* numeric_probe=nullptr){const auto pixels=presentation.readback_color();
        require(pixels.size()==size_t(width)*height*4,"DEC3N readback finishes at the full expected extent");
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){const uint32_t cell=(y/tile)*columns+x/tile;const size_t at=(size_t(y)*width+x)*4;
            const bool covered=cell<cells;
            if(covered && !(pixels[at]==tags[cell] && pixels[at+1]==255 && pixels[at+2]==0 && pixels[at+3]==255)){
                std::cerr<<"GPU_DEC3N_EXACT_MISMATCH cell="<<cell<<" x="<<x<<" y="<<y<<" BGRA="<<unsigned(pixels[at])<<','<<unsigned(pixels[at+1])<<','<<unsigned(pixels[at+2])<<','<<unsigned(pixels[at+3])<<'\n';
                if(numeric_probe)try {
                    for(const float selector:{-1.0f,-2.0f}){
                        Probe diagnostic=*numeric_probe;
                        const auto& color=element(diagnostic.draw.elements,19);
                        require(color.semanticName && std::string_view(color.semanticName)=="BLENDWEIGHT" && color.semanticIndex==0 &&
                                color.slotIndex==0 && color.format==plume::RenderFormat::R32_FLOAT &&
                                color.alignedByteOffset+4<=diagnostic.draw.stride,"numeric bit dump uses the original tag field");
                        for(uint32_t v=0;v<diagnostic.draw.vertex_count;++v)
                            std::memcpy(diagnostic.payload.data()+size_t(v)*diagnostic.draw.stride+color.alignedByteOffset,&selector,4);
                        presentation.clear(clear);paint(diagnostic);
                        const auto dumped=presentation.readback_color();
                        require(dumped.size()==pixels.size(),"numeric bit dump preserves the original full readback extent");
                        const uint32_t bits=uint32_t(dumped[at+2])|(uint32_t(dumped[at+1])<<8)|
                                            (uint32_t(dumped[at])<<16)|(uint32_t(dumped[at+3])<<24);
                        const auto flags=std::cerr.flags();
                        std::cerr<<"GPU_DEC3N_NUMERIC_BITS role="<<(selector==-1.0f?"decoded":"hardware_snorm16")
                                 <<" cell="<<cell<<" x="<<x<<" y="<<y<<" bits=0x"<<std::hex<<bits<<std::dec
                                 <<" BGRA="<<unsigned(dumped[at])<<','<<unsigned(dumped[at+1])<<','
                                 <<unsigned(dumped[at+2])<<','<<unsigned(dumped[at+3])<<'\n';
                        std::cerr.flags(flags);
                    }
                } catch(const std::exception& error) {std::cerr<<"GPU_DEC3N_NUMERIC_DUMP_ERROR "<<error.what()<<'\n';}
                throw std::runtime_error("GPU DEC3N differs bitwise from CPU-table/hardware-SNORM16 or an owned queued version");}
            if(!covered)require(pixels[at]==0 && pixels[at+1]==0 && pixels[at+2]==0 && pixels[at+3]==0,"unused DEC3N tiles retain clear pixels");
        }
    };
    unsigned cases=0;
    std::vector<uint32_t> corpus;corpus.reserve(3104);
    for(uint32_t component=0;component<3;++component)for(uint32_t bits=0;bits<1024;++bits){
        uint32_t lane[3]={513,127,400};lane[component]=bits;corpus.push_back(lane[0]|(lane[1]<<10)|(lane[2]<<20));}
    constexpr uint32_t triples[][3]={{0,0,0},{511,511,511},{512,512,512},{513,513,513},{1023,1,512},{1,1023,511},{0,511,512},{512,513,1023}};
    for(const auto& triple:triples)for(uint32_t high=0;high<4;++high)
        corpus.push_back(triple[0]|(triple[1]<<10)|(triple[2]<<20)|(high<<30));
    require(corpus.size()==3104,"three exhaustive component sweeps plus32 high-bit/mixed canaries");
    presentation.clear(clear);auto numeric=make(corpus,16,{{0,4}});
    require(numeric.mask==(raw?1u:0u),"backend selects actual raw decoder or legacy CPU fallback");
    paint(numeric);check(3104,std::vector<uint8_t>(3104,0),&numeric);++cases;
    if(raw){auto legacy=numeric;legacy.draw.vertex_shader=legacy_vertex.get();legacy.draw.vertex_dec3n_mask=0;
        auto& normal=element(legacy.draw.elements,4);normal.format=plume::RenderFormat::R16G16B16A16_SNORM;normal.slotIndex=0;normal.alignedByteOffset=16;
        presentation.clear(clear);paint(legacy);check(3104,std::vector<uint8_t>(3104,0));++cases;}
    for(uint32_t bit=0;bit<9;++bit){presentation.clear(clear);auto route=make({0xdff80200},16,{{bit,4}});
        require(route.mask==(raw?1u<<bit:0u),"each semantic uses exactly its specialization bit");
        paint(route);check(1,{0});++cases;}
    std::vector<std::pair<uint32_t,uint32_t>> all_fields;std::vector<uint32_t> all_words;
    for(uint32_t bit=0;bit<9;++bit){all_fields.push_back({bit,bit*4});all_words.push_back((bit*73u)|((1023-bit*71u)<<10)|((bit*109u)<<20));}
    presentation.clear(clear);auto multiple=make(all_words,36,all_fields);
    require(multiple.mask==(raw?0x1ffu:0u),"all nine fields share a single specialization without narrowing raw stride");
    paint(multiple);check(1,{0});++cases;
    for(auto order:{true,false}){
        presentation.clear(clear);auto phased=make({0xe007fdff},20,{{4,4}},order,4);paint(phased);check(1,{0});++cases;
    }
    presentation.clear(clear);auto unsorted=make({0x2007fdff,0x1ff00200},32,{{8,28},{2,12}});paint(unsorted);check(1,{0});++cases;
    presentation.clear(clear);auto shared=make({0x1ff80000,0x1ff80000},16,{{1,4},{6,4}});paint(shared);check(1,{0});++cases;
    for(const auto& domain:std::array<std::pair<uint32_t,uint32_t>,2>{{{16,2},{18,4}}}){
        presentation.clear(clear);auto declined=make({0xc007fe00},domain.first,{{0,domain.second}});
        require(!declined.mask,"under-aligned packed field or stride uses whole-layout legacy fallback");
        paint(declined);check(1,{0});++cases;
    }
    // Keep Normal0 packed on D3D12 while Normal1 retains each ordinary IA format.
    for(auto format:{plume::RenderFormat::R32G32B32A32_FLOAT,plume::RenderFormat::R16G16B16A16_SNORM,plume::RenderFormat::R16G16B16A16_UNORM}){
        presentation.clear(clear);auto ordinary=make({0x1ff80000},16,{{0,4}});
        require(ordinary.draw.vertex_dec3n_mask==(raw?1u:0u),"ordinary field is tested beside an actually active packed variant");
        const uint32_t size=format==plume::RenderFormat::R32G32B32A32_FLOAT?16u:8u,offset=ordinary.draw.stride;
        const auto original=std::move(ordinary.payload);ordinary.draw.stride+=size;
        ordinary.payload.resize(6*ordinary.draw.stride);const std::array<float,4> value{-0.25f,0.5f,0.75f,1};
        const std::array<int16_t,4> snorm{-8192,16384,24575,32767};const std::array<uint16_t,4> unorm{0,16384,49151,65535};
        for(unsigned v=0;v<6;++v){auto* to=ordinary.payload.data()+v*ordinary.draw.stride;
            std::memcpy(to,original.data()+v*offset,offset);std::memcpy(to+offset,
                format==plume::RenderFormat::R32G32B32A32_FLOAT?static_cast<const void*>(value.data()):
                format==plume::RenderFormat::R16G16B16A16_SNORM?static_cast<const void*>(snorm.data()):static_cast<const void*>(unorm.data()),size);}
        for(uint32_t location:{5u,1u}){auto& e=element(ordinary.draw.elements,location);e.format=format;e.slotIndex=0;e.alignedByteOffset=offset;}
        paint(ordinary);check(1,{0});++cases;
    }
    // Use only upstream's own cache/ring retirement; no fork manager or snapshot API.
    struct RestoreDirect{bool value;~RestoreDirect(){sfr::GuestMemory::direct_guest_access=value;}} restore{sfr::GuestMemory::direct_guest_access};
    sfr::GuestMemory::direct_guest_access=false;sfr::GuestMemory memory;
    constexpr uint32_t backing=0xe0030000,physical=0x31000;memory.map(backing,4096);
    auto old=make({0x2007fdff},16,{{0,4}},true,0,64);
    auto newer=make({0xdff80000},16,{{0,4}},false,0,128);
    const auto fill=[&](Probe& probe){
        for(size_t at=0;at<probe.payload.size();++at)memory.store<uint8_t>(backing+uint32_t(at),probe.payload[at]);
        memory.advance_write_epoch();
        const auto first=renderer.vertex_cache(memory,physical,probe.payload.size(),probe.payload.size(),probe.draw.stride);
        require(!first.buffer,"cold or written source uses ordinary cache admission");
        memory.advance_write_epoch();
        const auto value=renderer.vertex_cache(memory,physical,probe.payload.size(),probe.payload.size(),probe.draw.stride);
        require(value.buffer && value.fill.size()==probe.payload.size(),"one unwritten epoch admits actual upstream cache fill");
        std::memcpy(value.fill.data(),probe.payload.data(),probe.payload.size());
        probe.draw.vertices={};probe.draw.vertex_buffer=value.buffer;
        const auto hit=renderer.vertex_cache(memory,physical,probe.payload.size(),probe.payload.size(),probe.draw.stride);
        require(hit.buffer==value.buffer && hit.fill.empty(),"filled same-version cache is a real ready hit");
    };
    const auto queued=[&](Probe& probe,uint32_t column){presentation.set_raster_state({float(column*tile),0,width,height},
        {int(column*tile),0,int((column+1)*tile),int(tile)});renderer.draw(probe.draw);};
    presentation.clear(clear);fill(old);queued(old,0);fill(newer);queued(newer,1);
    auto legacy=newer;legacy.draw.vertex_shader=legacy_vertex.get();legacy.draw.vertex_dec3n_mask=0;
    const uint32_t previous_stride=legacy.draw.stride;legacy.draw.stride+=8;
    legacy.payload.resize(size_t(legacy.draw.vertex_count)*legacy.draw.stride);
    for(uint32_t v=0;v<legacy.draw.vertex_count;++v){auto* to=legacy.payload.data()+size_t(v)*legacy.draw.stride;
        const auto* from=newer.payload.data()+size_t(v)*previous_stride;std::memcpy(to,from,previous_stride);
        std::memcpy(to+previous_stride,from+16,8);}
    auto& legacy_normal=element(legacy.draw.elements,4);legacy_normal.format=plume::RenderFormat::R16G16B16A16_SNORM;
    legacy_normal.slotIndex=0;legacy_normal.alignedByteOffset=previous_stride;
    fill(legacy);queued(legacy,2);queued(old,3);
    old.payload.assign(old.payload.size(),0xdd);newer.payload.assign(newer.payload.size(),0xee);
    legacy.payload.assign(legacy.payload.size(),0xcc);memory.release(backing,4096);
    check(4,{64,128,128,64});++cases;
    // The first readback consumed one flush; this repeat is within the existing +2 retirement margin.
    presentation.clear(clear);queued(legacy,0);queued(old,1);check(2,{128,64});++cases;
    std::cout<<"GPU_DEC3N passed backend="<<sfr::graphics_backend_name(graphics.backend())<<" raw="<<raw
             <<" values=3104 routing=9 cases="<<cases<<" exact_snorm=passed w1=passed queued=passed fallback=passed readback=finished\n";
}

void run(bool gpu_dec3n=false) {
    environment("SFR_GPU_PIPELINE", "1");
    const fs::path directory = fs::temp_directory_path() / ("sfr-resolution-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{directory};
    const char* common=gpu_dec3n?std::getenv("SFR_TEST_SHADER_COMMON"):nullptr;
    auto header = sfr::resolution_shader_header(read(common && *common ? fs::path(common) :
        fs::path(SFR_SOURCE_DIR) / "tools/XenosRecomp/XenosRecomp/shader_common.h"));
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
    if(gpu_dec3n){gpu_dec3n_readback(graphics,directory,header);return;}
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
int main(int argc,char** argv) {
    try {
        require(argc==1 || (argc==2 && std::string_view(argv[1])=="--gpu-dec3n"),"unknown resolution fixture option");
        run(argc==2); std::cout << "Internal resolution readback checks passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
