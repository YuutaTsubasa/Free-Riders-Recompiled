#include "native_graphics.h"
#include "native_presentation.h"
#include "native_renderer.h"
#include "guest_memory.h"
#include "physical_memory.h"
#include "word_swap.h"
#include "guest_buffer_state.h"
#include "guest_graphics.h"
#include "graphics_defaults_fixture.h"
#include "original_loop_setter_fixture.h"
#include "original_stencil_setter_fixture.h"
#include "native_render_state.h"
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
#include <utility>
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
        environment("SFR_TEXTURE_TIMERS", batch ? "1" : "0");
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
            require(before.texture_timers == batch &&
                    (batch ? before.texture_milliseconds > 0 : before.texture_milliseconds == 0),
                    "explicit texture timing preserves availability; disabled timing stays unavailable");
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
    environment("SFR_TEXTURE_TIMERS", "0");
}

void prepared_texture_bindings_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
float4 shaderMain(uint id : SV_VertexID) : SV_Position {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 return float4(p[id],0.5,1);
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain() : SV_Target { return tfetch2D(textures0,samplers0,float2(0.5,0.5),float2(0,0)); }
)", false);
    environment("SFR_RENDER_SCALE", "100");
    sfr::GuestMemory memory;
    constexpr uint32_t params = 0x10000000, output = params+124, texture_a = params+128, texture_b = params+256;
    constexpr uint32_t device = sfr::GuestGraphics::device_address, physical_a = 0x101000, physical_b = 0x104000;
    memory.map(params, 0x1000); memory.map(0x82AD0000, 0x2000); memory.map(0xA0100000, 0x10000);
    constexpr std::array<uint32_t,31> parameters{
        16,16,0x18280186,1,0,0,1,0,0,1,0x1A220197,0,0,1,0,0,0x28280106,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    for (size_t i=0; i<parameters.size(); ++i) memory.store<uint32_t>(params+i*4, parameters[i]);
    for (auto [table,count] : {std::pair{0x82AD0A60u,101u}, std::pair{0x82AD0F20u,20u}})
        for (uint32_t i=0; i<count; ++i) {
            memory.store<uint32_t>(table+i*12, 0x12340000+i);
            memory.store<uint32_t>(table+i*12+4, 0x82210000+i*4);
            memory.store<uint32_t>(table+i*12+8, i);
        }
    sfr::test::install_blend_defaults(memory); sfr::test::install_sampler_defaults(memory);
    sfr::GuestGraphics guest(memory, graphics);
    require(guest.create_device(0,1,0,0,params,output) == 0, "texture fixture creates the actual guest device");
    auto& presentation = guest.presentation(); auto& renderer = guest.renderer();
    for (auto [resource,physical] : {std::pair{texture_a,physical_a}, std::pair{texture_b,physical_b}}) {
        const sfr::FetchWords words{2u | (1u<<22), physical | 6u | (2u<<6), 0u, 0x688u<<1, 0u, 1u<<9};
        for (uint32_t i=0; i<6; ++i) memory.store<uint32_t>(resource+28+i*4, words[i]);
    }
    memory.store<uint32_t>(0xA0000000u | physical_a, 0xFF0000FF);
    memory.store<uint32_t>(0xA0000000u | physical_b, 0xFF00FF00);
    sfr::NativeDraw draw;
    draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
    sfr::NativeClear clear{}; clear.color = true; presentation.clear(clear);
    const auto paint = [&](int left, int right, bool managed = true) {
        const auto work = guest.prepare_texture_bindings(draw.shared, managed, false);
        presentation.set_raster_state({0,0,16,16}, {left,0,right,16});
        renderer.draw(draw);
        return work;
    };
    const auto check = [](const std::vector<uint8_t>& pixels, const std::array<std::array<uint8_t,3>,4>& columns) {
        require(pixels.size() == 16*16*4, "prepared texture readback has its requested extent");
        for (size_t at=0; at<pixels.size(); at+=4) {
            const auto& rgb = columns[(at/4%16)/4];
            require(pixels[at+2] == rgb[0] && pixels[at+1] == rgb[1] && pixels[at] == rgb[2] && pixels[at+3] == 255,
                    "prepared descriptors preserve the texture version captured by each draw");
        }
    };
    guest.set_texture(device, 0, texture_a, 0x80000000ull);
    require(paint(0,4).read_bytes == 384, "first prepared texture draw reads every sampled fetch");
    const auto clean = paint(4,8);
    require(clean.read_bytes == 0 && clean.descriptor_lookups == 0 && clean.descriptor_hits == 16,
            "clean texture draw reuses all descriptors without reading guest fetches");
    guest.set_texture(device, 0, texture_b, 0x80000000ull);
    require(paint(8,12).read_bytes == 24, "binding a different resource refreshes just its fetch slot");
    const auto dirty = memory.load<uint64_t>(device+24);
    guest.set_texture(device, 0, 0, 0x80000000ull);
    require(memory.load<uint64_t>(device+24) == dirty, "NULL retains the original setter's guest dirty behavior");
    require(paint(12,16).read_bytes == 24 && (guest.texture_state().slot(0).words[0]&3) == 0,
            "NULL invalidates the shadow while its type-zero fetch remains sampleable");
    check(presentation.readback_color(), {{{255,0,0},{255,0,0},{0,255,0},{0,255,0}}});

    // A frame boundary invalidates descriptor memoization, not the fetch shadow.
    const auto next_frame = paint(0,8);
    require(next_frame.read_bytes == 0 && next_frame.descriptor_lookups == 1,
            "new renderer generation refreshes descriptors without rereading clean fetch words");
    const auto old_descriptor = draw.shared.texture_2d[0];
    const auto generation = renderer.texture_generation();
    memory.store<uint32_t>(0xA0000000u | physical_b, 0xFFFF0000);
    renderer.invalidate(physical_b, 128);
    require(renderer.texture_generation() != generation, "content retirement advances descriptor generation");
    const auto rewritten = paint(8,16);
    require(rewritten.read_bytes == 0 && rewritten.descriptor_lookups == 1 && draw.shared.texture_2d[0] != old_descriptor,
            "content change replaces a queued descriptor without changing guest fetch words");
    check(presentation.readback_color(), {{{0,255,0},{0,255,0},{0,0,255},{0,0,255}}});

    guest.set_texture(device, 0, texture_a, 0x80000000ull);
    require(paint(0,8,false).read_bytes == 384, "legacy preparation reads the original full fetch block");
    paint(8,12);
    const auto managed_again = paint(12,16);
    require(managed_again.read_bytes == 0 && managed_again.descriptor_hits == 16,
            "managed preparation becomes clean again after a legacy draw");
    check(presentation.readback_color(), {{{255,0,0},{255,0,0},{255,0,0},{255,0,0}}});
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

void managed_vertex_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
struct Output { float4 position : SV_Position; float red : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID, float red : POSITION0) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position=float4(p[id],0.5,1); o.red=red; return o;
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain(float4 position : SV_Position, float red : TEXCOORD0) : SV_Target {
 return float4(red,0.5,0.25,1);
})", false);
    environment("SFR_RENDER_SCALE", "100");
    sfr::GuestMemory memory;
    sfr::GuestBufferState state(memory);
    constexpr uint32_t resource=0x10000, backing=0xe0010000, physical=0x11000;
    memory.map(resource,4096); memory.map(backing,4096);
    const auto create=[&] {
        memory.store<uint32_t>(resource,0x100001);
        memory.store<uint32_t>(resource+24,backing|3);
        memory.store<uint32_t>(resource+28,24|0x10000002);
        state.created(resource);
    };
    const auto update=[&](float a,float b) {
        state.lock(resource);
        for(uint32_t i=0;i<6;++i) memory.store<uint32_t>(backing+i*4,std::bit_cast<uint32_t>(i<3?a:b));
        state.unlock(resource,0x100);
        return state.lookup(resource,physical,24,true);
    };
    sfr::NativePresentation presentation(graphics,16,16);
    sfr::NativeRenderer renderer(graphics,presentation);
    sfr::NativeDraw draw;
    draw.vertex_count=3; draw.stride=4; draw.vertex_shader=vertex.get(); draw.pixel_shader=pixel.get();
    draw.elements.emplace_back("POSITION",0,0,plume::RenderFormat::R32_FLOAT,0,0);
    create();
    auto old=update(0.125f,0.25f);
    (void)renderer.take_pipeline_work();
    draw.vertex_buffer={renderer.managed_vertex_buffer(old),12};
    const auto initial_work=renderer.take_pipeline_work();
    require(initial_work.managed_vertex_lookups==1 && initial_work.managed_vertex_allocations==1,
            "first static binding resolves and allocates its immutable version");
    require(draw.vertex_buffer.buffer && renderer.managed_vertex_buffer(old)==draw.vertex_buffer.buffer,"one GPU allocation per immutable version");
    const auto repeated_work=renderer.take_pipeline_work();
    require(repeated_work.managed_vertex_lookups==0 && repeated_work.managed_vertex_allocations==0,
            "adjacent static bindings avoid map lookup and allocation");
    // Bind the second slice, then replace and free it before the render worker
    // necessarily records the first draw. Both draws must keep their versions.
    presentation.set_raster_state({0,0,16,16},{0,0,8,16});
    renderer.draw(draw);
    state.released(resource,state.identity(resource)); old.reset();
    create(); auto newer=update(0.75f,0.875f);
    draw.vertex_buffer={renderer.managed_vertex_buffer(newer)};
    presentation.set_raster_state({0,0,16,16},{8,0,16,16});
    renderer.draw(draw);
    state.released(resource,state.identity(resource)); newer.reset();
    const auto pixels=presentation.readback_color();
    for(uint32_t y=0;y<16;++y) for(uint32_t x=0;x<16;++x) {
        const size_t at=(y*16+x)*4;
        require(std::abs(int(pixels[at+2])-(x<8?64:191))<=1 && std::abs(int(pixels[at+1])-128)<=1,
                "queued draws retain old/new buffers, subrange offsets and colors after release");
    }
    // The production hook reuses one NativeDraw across managed slices, the
    // legacy cache (including DEC3N) and upload-ring draws. Replacing only its
    // buffer used to leave a slice offset behind and could fault the GPU.
    constexpr uint32_t legacy_address=0xe0020000, legacy_physical=0x21000;
    memory.map(legacy_address,4096);
    for(uint32_t i=0;i<3;++i) memory.store<uint32_t>(legacy_address+i*4,std::bit_cast<uint32_t>(0.5f));
    renderer.vertex_cache(memory,legacy_physical,memory.base()+legacy_address,12,12,4);
    memory.advance_write_epoch();
    auto legacy=renderer.vertex_cache(memory,legacy_physical,memory.base()+legacy_address,12,12,4);
    require(legacy.buffer && legacy.fill.size()==12,"legacy cache fixture allocates a vertex slice");
    const std::array<float,3> legacy_values{0.5f,0.5f,0.5f}, ring_values{0.75f,0.75f,0.75f};
    std::memcpy(legacy.fill.data(),legacy_values.data(),12);
    create(); auto mixed=update(0.125f,0.25f);
    const auto* managed=renderer.managed_vertex_buffer(mixed);
    for(int column=0;column<4;++column) {
        if(column==0) draw.vertex_buffer={managed,12};
        else if(column==2) {
            draw.vertex_buffer={};
            draw.vertices={reinterpret_cast<const uint8_t*>(ring_values.data()),12};
        } else draw.vertex_buffer={legacy.buffer,legacy.offset};
        presentation.set_raster_state({0,0,16,16},{column*4,0,(column+1)*4,16});
        renderer.draw(draw);
    }
    const auto alternating=presentation.readback_color();
    constexpr int red_by_column[]={64,128,191,128};
    for(uint32_t y=0;y<16;++y) for(uint32_t x=0;x<16;++x)
        require(std::abs(int(alternating[(y*16+x)*4+2])-red_by_column[x/4])<=1,
                "managed, legacy-cache and ring bindings cannot inherit another draw's offset");
    state.released(resource,state.identity(resource)); mixed.reset();
    // Retire released versions while subsequent asynchronous frames use new
    // allocations at the same guest header and backing addresses.
    for(int frame=0;frame<6;++frame) {
        create(); auto next=update(0.5f,0.625f);
        draw.vertex_buffer={renderer.managed_vertex_buffer(next)};
        presentation.set_raster_state({0,0,16,16},{0,0,16,16});
        renderer.draw(draw);
        state.released(resource,state.identity(resource)); next.reset();
        presentation.present();
    }
    const auto final=presentation.readback_color();
    require(std::abs(int(final[2])-128)<=1,"GPU reads current data after retired versions are collected");

    // Keep the CPU version alive while exercising real static-buffer aging.
    // Empty flushes invoke retirement without submitting 600 GPU frames.
    for(unsigned i=0;i<4;++i) presentation.flush();
    create(); auto retained=update(0.125f,0.625f);
    draw.vertex_buffer={renderer.managed_vertex_buffer(retained),12};
    require(draw.vertex_buffer.buffer!=nullptr,"retained static version is uploaded");
    (void)renderer.take_pipeline_work();
    for(unsigned i=0;i<600;++i) presentation.flush();
    require(renderer.managed_vertex_buffer(retained)==draw.vertex_buffer.buffer,
            "static version survives through its idle retention boundary");
    presentation.flush();
    require(renderer.managed_vertex_buffer(retained)==draw.vertex_buffer.buffer,
            "memo hit touches the static entry before the next retirement pass");
    const auto touched=renderer.take_pipeline_work();
    require(touched.managed_vertex_lookups==0 && touched.managed_vertex_allocations==0,
            "static memo hits retain the original allocation across flushes");
    for(unsigned i=0;i<601;++i) presentation.flush();
    draw.vertex_buffer={renderer.managed_vertex_buffer(retained),12};
    require(draw.vertex_buffer.buffer && renderer.managed_vertex_buffer(retained)==draw.vertex_buffer.buffer,
            "evicted static version can be uploaded and reused again");
    const auto recreated=renderer.take_pipeline_work();
    require(recreated.managed_vertex_lookups==1 && recreated.managed_vertex_allocations==1,
            "static eviction invalidates the borrowed memo before the same version is rebound");
    renderer.draw(draw);
    const auto rebound=presentation.readback_color();
    require(std::abs(int(rebound[2])-159)<=1,"recreated static allocation keeps its nonzero subrange offset");
    state.released(resource,state.identity(resource)); retained.reset();

    // Dynamic slices share the draw ring, not one GPU allocation per update.
    // Test subranges, reuse, wrap, release and both synchronous/asynchronous
    // ring retirement while earlier commands still reference their contents.
    create();
    const auto dynamic_update=[&](float red) {
        memory.store<uint32_t>(resource,0x100101);
        state.begin_range(resource,physical+12,12);
        for(uint32_t i=3;i<6;++i) memory.store<uint32_t>(backing+i*4,std::bit_cast<uint32_t>(red));
        memory.store<uint32_t>(resource,0x100001);
        state.finish_ranges(resource,0x100);
        return state.lookup(resource,physical+12,12,true);
    };
    draw.vertex_buffer={}; draw.vertices={}; draw.vertex_snapshot_offset=0;
    draw.vertex_snapshot=dynamic_update(0.25f);
    require(bool(draw.vertex_snapshot),"completed dynamic slice is available to the renderer");
    const auto first_dynamic=draw.vertex_snapshot;
    (void)renderer.take_pipeline_work();
    for(int column=0;column<4;++column) {
        if(column==2) draw.vertex_snapshot=dynamic_update(0.75f);
        presentation.set_raster_state({0,0,16,16},{column*4,0,(column+1)*4,16});
        renderer.draw(draw);
    }
    const auto dynamic_work=renderer.take_pipeline_work();
    require(dynamic_work.draw_ring_bytes==4*(8192+768)+2*256 && dynamic_work.transient_vertex_lookups==2,
            "each dynamic version uploads once despite multiple draws");
    const auto latest_dynamic=draw.vertex_snapshot;
    draw.vertex_snapshot=first_dynamic;
    presentation.set_raster_state({0,0,16,16},{0,0,4,16});
    renderer.draw(draw); renderer.draw(draw);
    const auto restored_dynamic=renderer.take_pipeline_work();
    require(restored_dynamic.transient_vertex_lookups==1 && restored_dynamic.draw_ring_bytes==2*(8192+768),
            "returning to an older dynamic version finds its ring allocation once, then memoizes it");
    draw.vertex_snapshot=latest_dynamic;
    state.released(resource,state.identity(resource));
    const auto dynamic=presentation.readback_color();
    for(uint32_t y=0;y<16;++y) for(uint32_t x=0;x<16;++x)
        require(std::abs(int(dynamic[(y*16+x)*4+2])-(x<8?64:191))<=1,
                "dynamic wrap and release keep older queued draws intact");
    for(int frame=0;frame<4;++frame) {
        presentation.set_raster_state({0,0,16,16},{0,0,16,16});
        renderer.draw(draw);
        renderer.draw(draw);
        const auto work=renderer.take_pipeline_work();
        require(work.draw_ring_bytes==2*(8192+768)+256 && work.transient_vertex_lookups==1,
                "new ring reuploads the slice once before reusing it without another lookup");
        presentation.present();
    }
    renderer.draw(draw);
    (void)renderer.take_pipeline_work();
    require(!renderer.vertex_space((64ull<<20)-32768,0).empty(),"dynamic near-capacity reservation fits");
    renderer.draw(draw);
    const auto capacity_work=renderer.take_pipeline_work();
    require(capacity_work.ring_flushes==1 && capacity_work.transient_vertex_lookups==1 &&
            capacity_work.draw_ring_bytes==8192+768+256,
            "capacity reservation resets the dynamic memo and uploads the same version again");
    const auto after_flush=presentation.readback_color();
    require(std::abs(int(after_flush[2])-191)<=1,"capacity flush preserves the dynamic snapshot");
}

void index_cache_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
struct Output { float4 position : SV_Position; float red : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position=float4(p[id % 3],0.5,1); o.red=0.125 + 0.25 * (id / 3); return o;
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain(float4 position : SV_Position, float red : TEXCOORD0) : SV_Target {
 return float4(red,0.5,0.25,1);
})", false);
    environment("SFR_RENDER_SCALE", "100");
    sfr::GuestMemory memory;
    constexpr uint32_t backing=0xe0030000, physical=0x31000;
    constexpr uint32_t alias_a=0xa0031000, alias_c=0xc0031000, tail=4090;
    memory.map(backing,4096); memory.map(alias_a,4096); memory.map(alias_c,4096);
    const std::array<uint32_t,3> first{0,1,2}, second{3,4,5}, third{6,7,8}, fourth{9,10,11};
    const std::array<uint32_t,3> wrapped{0xfffffffdu,0xfffffffeu,0xffffffffu};
    const std::array<uint32_t,6> two_triangles{0,1,2,9,10,11};
    const auto put = [&](uint32_t view, uint32_t offset, std::span<const uint32_t> values, bool wide=false) {
        for(uint32_t i=0;i<values.size();++i) {
            if(wide) memory.store<uint32_t>(view+offset+i*4,values[i]);
            else memory.store<uint16_t>(view+offset+i*2,uint16_t(values[i]));
        }
    };
    for(const auto view : {backing,alias_a,alias_c}) {
        put(view,tail,first); // Three BE16 indices end exactly at the mapped page boundary.
        put(view,2,second);   // A slice need not be aligned to four bytes.
        put(view,16,third,true);
        put(view,32,wrapped,true);
        put(view,64,two_triangles);
        put(view,96,third);
    }
    sfr::NativePresentation presentation(graphics,16,16);
    sfr::NativeDraw draw;
    const std::array<uint32_t,12> unused_vertices{};
    draw.vertex_count=12; draw.stride=4;
    draw.vertices={reinterpret_cast<const uint8_t*>(unused_vertices.data()),sizeof(unused_vertices)};
    draw.vertex_shader=vertex.get(); draw.pixel_shader=pixel.get(); draw.index_count=3;
    const auto fetch = [&](sfr::NativeRenderer& owner, uint32_t offset, uint32_t count=3,
                           bool wide=false, uint32_t base=0, uint32_t view=0) {
        const uint32_t address=(view ? view : backing)+offset;
        memory.check(address,uint64_t(count)*(wide ? 4 : 2));
        return owner.index_cache(memory,physical+offset,memory.base()+address,count,wide,base);
    };
    const auto scan = [&](const sfr::NativeRenderer::CachedIndices& cached, uint32_t lowest, uint32_t highest) {
        require(cached.buffer && cached.scan.lowest==lowest && cached.scan.highest==highest && !cached.scan.restart,
                "cached IndexScan belongs to the exact source width, slice and adjusted GPU indices");
    };
    const auto unavailable = [&](const sfr::NativeRenderer::CachedIndices& cached) {
        require(!cached.buffer && cached.scan.lowest>cached.scan.highest && !cached.scan.restart,
                "an unready or retired index allocation cannot expose a ready scan");
    };
    const auto work = [&](sfr::NativeRenderer& owner, uint64_t decodes, uint64_t allocations) {
        const auto actual=owner.take_pipeline_work();
        require(actual.index_cache_decodes==decodes && actual.index_cache_allocations==allocations,
                "index cache performs decode and allocation only on successful admissions");
    };
    const auto paint = [&](sfr::NativeRenderer& owner, const sfr::NativeRenderer::CachedIndices& cached,
                           int left, int right) {
        require(cached.buffer!=nullptr,"readback binds a completed index allocation");
        draw.index_buffer=cached.buffer;
        presentation.set_raster_state({0,0,16,16},{left,0,right,16});
        owner.draw(draw);
    };
    const auto check = [&](const std::array<int,4>& reds) {
        const auto pixels=presentation.readback_color();
        require(pixels.size()==16*16*4,"index readback has its requested extent");
        for(uint32_t y=0;y<16;++y) for(uint32_t x=0;x<16;++x) {
            const auto at=(y*16+x)*4;
            require(std::abs(int(pixels[at+2])-reds[x/4])<=1 &&
                    std::abs(int(pixels[at+1])-128)<=1 && std::abs(int(pixels[at])-64)<=1 && pixels[at+3]==255,
                    "GPU readback selects the vertices represented by each immutable index buffer and scan");
        }
    };
    {
        sfr::NativeRenderer renderer(graphics,presentation);
        unavailable(fetch(renderer,tail)); unavailable(fetch(renderer,2));
        unavailable(fetch(renderer,16,3,true)); unavailable(fetch(renderer,32,3,true,3));
        work(renderer,0,0); // First access watches; it does not decode or allocate.
        memory.advance_write_epoch();
        const auto a=fetch(renderer,tail), b=fetch(renderer,2), c=fetch(renderer,16,3,true), d=fetch(renderer,32,3,true,3);
        scan(a,0,2); scan(b,3,5); scan(c,6,8); scan(d,0,2);
        work(renderer,4,4);
        for(unsigned i=0;i<32;++i) {
            const auto hit=fetch(renderer,tail);
            scan(hit,0,2);
            require(hit.buffer==a.buffer,"repeat hits retain their original completed allocation");
        }
        work(renderer,0,0);
        paint(renderer,a,0,4); paint(renderer,b,4,8); paint(renderer,c,8,12); paint(renderer,d,12,16);
        check({32,96,159,32});

        // Same source address with a new base, and equal byte extents with
        // different counts/widths, must retain separate data and scan keys.
        unavailable(fetch(renderer,tail,3,false,3)); unavailable(fetch(renderer,2,3,false,0xfffffffdu));
        unavailable(fetch(renderer,64,3)); unavailable(fetch(renderer,64,6)); unavailable(fetch(renderer,64,3,true));
        memory.advance_write_epoch();
        const auto based=fetch(renderer,tail,3,false,3), wrap16=fetch(renderer,2,3,false,0xfffffffdu);
        scan(based,3,5); scan(wrap16,0,2);
        scan(fetch(renderer,64,3),0,2); scan(fetch(renderer,64,6),0,11);
        scan(fetch(renderer,64,3,true),1,0x000a000bu);
        work(renderer,5,5);
        paint(renderer,based,0,4); paint(renderer,wrap16,4,8); paint(renderer,c,8,12); paint(renderer,d,12,16);
        check({96,32,159,32});

        constexpr uint64_t layout=uint64_t(1)<<63; // The index tag for BE16/base zero.
        auto vertex_slice=renderer.vertex_cache(memory,physical+tail,memory.base()+backing+tail,6,12,layout,false);
        require(!vertex_slice.buffer,"an equal vertex layout tag cannot reuse an index allocation");
        memory.advance_write_epoch();
        vertex_slice=renderer.vertex_cache(memory,physical+tail,memory.base()+backing+tail,6,12,layout,false);
        require(vertex_slice.buffer && vertex_slice.fill.size()==12 && vertex_slice.buffer!=a.buffer,
                "vertex and index cache namespaces keep different buffer flags and ownership");
        std::memcpy(vertex_slice.fill.data(),fourth.data(),12);
        require(fetch(renderer,tail).buffer==a.buffer,"vertex admission cannot displace the live index scan");
        work(renderer,0,0);

        // Exercise a real uncommitted low-level fill. It may already be queued
        // by its caller, so index_cache must retire it rather than overwrite it
        // or fabricate metadata. The replacement reads the checked guest bytes.
        require(!renderer.vertex_cache(memory,physical+96,memory.base()+backing+96,6,12,layout,true).buffer,
                "low-level index fill follows the normal first-watch admission");
        memory.advance_write_epoch();
        const auto pending=renderer.vertex_cache(memory,physical+96,memory.base()+backing+96,6,12,layout,true);
        require(pending.buffer && pending.fill.size()==12,"low-level fixture obtains an uncommitted index allocation");
        std::memcpy(pending.fill.data(),fourth.data(),12);
        draw.index_buffer=pending.buffer;
        presentation.set_raster_state({0,0,16,16},{0,0,8,16});
        renderer.draw(draw);
        unavailable(fetch(renderer,96));
        work(renderer,0,0);
        memory.advance_write_epoch();
        const auto committed=fetch(renderer,96);
        scan(committed,6,8); work(renderer,1,1);
        paint(renderer,committed,8,16);
        check({223,223,159,159});

        // The fixture maps E/A/C separately: only the alias is modified and
        // passed as the next checked source, so E's unchanged page cannot hide
        // a missing alias guard. No submission occurs between old/new draws.
        auto previous=fetch(renderer,tail);
        for(const auto [view,values] : {std::pair{alias_a,second},std::pair{alias_c,third}}) {
            paint(renderer,previous,0,8);
            put(view,tail,values);
            unavailable(fetch(renderer,tail,3,false,0,view));
            work(renderer,0,0);
            memory.advance_write_epoch();
            unavailable(fetch(renderer,tail,3,false,0,view)); // Retains the existing conservative epoch policy.
            memory.advance_write_epoch();
            const auto updated=fetch(renderer,tail,3,false,0,view);
            scan(updated,values.front(),values.back());
            work(renderer,1,1);
            require(fetch(renderer,tail,3,false,0,view).buffer==updated.buffer,
                    "refill publishes new metadata with the new GPU bytes");
            work(renderer,0,0);
            paint(renderer,updated,8,16);
            check(view==alias_a ? std::array<int,4>{32,32,96,96} : std::array<int,4>{96,96,159,159});
            previous=updated;
        }

        // Real cache retirement, without 65,536 independent GPU allocations.
        // Empty flush callbacks cross both the 600-frame age and sweep boundary.
        work(renderer,0,0);
        for(unsigned i=0;i<1200;++i) presentation.flush();
        unavailable(fetch(renderer,tail,3,false,0,alias_c));
        work(renderer,0,0);
        memory.advance_write_epoch();
        const auto recreated=fetch(renderer,tail,3,false,0,alias_c);
        scan(recreated,6,8); work(renderer,1,1);
        require(fetch(renderer,tail,3,false,0,alias_c).buffer==recreated.buffer,
                "aged-out index metadata and buffer are recreated and hit together");
        work(renderer,0,0);
        paint(renderer,recreated,0,16); check({159,159,159,159});
    }
    {
        // A new renderer must establish its own admission and metadata even
        // while the same guest memory and presentation still exist.
        sfr::NativeRenderer renderer(graphics,presentation);
        unavailable(fetch(renderer,tail,3,false,0,alias_c)); work(renderer,0,0);
        memory.advance_write_epoch();
        const auto recreated=fetch(renderer,tail,3,false,0,alias_c);
        scan(recreated,6,8); work(renderer,1,1);
        paint(renderer,recreated,0,16); check({159,159,159,159});
    }
}

void index_mapping_lifecycle_readback(sfr::NativeGraphics& graphics, const fs::path& directory,
                                      const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
struct Output { float4 position : SV_Position; float red : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position=float4(p[id % 3],0.5,1); o.red=0.125 + 0.25 * (id / 3); return o;
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain(float4 position : SV_Position, float red : TEXCOORD0) : SV_Target {
 return float4(red,0.5,0.25,1);
})", false);
    environment("SFR_RENDER_SCALE", "100");
    sfr::NativePresentation presentation(graphics,16,16);
    sfr::NativeDraw draw;
    const std::array<uint32_t,12> unused_vertices{};
    draw.vertex_count=12; draw.stride=4; draw.index_count=3;
    draw.vertices={reinterpret_cast<const uint8_t*>(unused_vertices.data()),sizeof(unused_vertices)};
    draw.vertex_shader=vertex.get(); draw.pixel_shader=pixel.get();
    const auto put = [](sfr::GuestMemory& memory, uint32_t address, uint32_t first) {
        for(uint32_t i=0;i<3;++i) memory.store<uint16_t>(address+i*2,uint16_t(first+i));
    };
    const auto fetch = [](sfr::NativeRenderer& renderer, sfr::GuestMemory& memory, uint32_t physical) {
        // Use the production caller's source selection, including E/A/C priority.
        const auto address=sfr::NativeRenderer::guest_address(memory,physical,6);
        memory.check(address,6);
        return renderer.index_cache(memory,physical,memory.base()+address,3,false,0);
    };
    const auto admit = [&](sfr::NativeRenderer& renderer, sfr::GuestMemory& memory,
                           uint32_t physical, uint32_t first) {
        for(unsigned attempt=0;attempt<3;++attempt) {
            memory.advance_write_epoch();
            const auto cached=fetch(renderer,memory,physical);
            if(!cached.buffer) continue;
            require(cached.scan.lowest==first && cached.scan.highest==first+2 && !cached.scan.restart,
                    "mapping lifecycle publishes the independent expected index scan");
            return cached;
        }
        throw std::runtime_error("mapping lifecycle did not admit a stable completed source");
    };
    const auto work = [](sfr::NativeRenderer& renderer, uint64_t fills) {
        const auto actual=renderer.take_pipeline_work();
        require(actual.index_cache_decodes==fills && actual.index_cache_allocations==fills,
                "mapping lifecycle decodes and allocates exactly once for each admitted replacement");
    };
    const auto paint = [&](sfr::NativeRenderer& renderer, const sfr::NativeRenderer::CachedIndices& cached,
                           int left, int right) {
        require(cached.buffer!=nullptr,"mapping lifecycle draws a completed index buffer");
        draw.index_buffer=cached.buffer;
        presentation.set_raster_state({0,0,16,16},{left,0,right,16});
        renderer.draw(draw);
    };
    const auto check = [&](int old_red, int new_red) {
        const auto pixels=presentation.readback_color();
        require(pixels.size()==16*16*4,"mapping lifecycle readback has its expected extent");
        for(uint32_t y=0;y<16;++y) for(uint32_t x=0;x<16;++x) {
            const auto at=(y*16+x)*4;
            require(std::abs(int(pixels[at+2])-(x<8 ? old_red : new_red))<=1 &&
                    std::abs(int(pixels[at+1])-128)<=1 && std::abs(int(pixels[at])-64)<=1 && pixels[at+3]==255,
                    "queued old/new index data survives backing changes until GPU readback");
        }
    };
    {
        sfr::GuestMemory memory;
        sfr::NativeRenderer renderer(graphics,presentation);
        constexpr uint32_t physical=0x41000, alias_a=0xa0041000, preferred_e=0xe0040000;
        memory.map(alias_a,4096);
        put(memory,alias_a,0);
        require(sfr::NativeRenderer::guest_address(memory,physical,6)==alias_a,
                "the initial source is the only readable A alias");
        require(!fetch(renderer,memory,physical).buffer,"A-only source first establishes its watches");
        const auto old=admit(renderer,memory,physical,0);
        work(renderer,1);
        paint(renderer,old,0,8);

        // Do not modify or remap A. Only introduce a preferred, independently
        // backed alias whose bytes disagree with the still-valid old A view.
        memory.map(preferred_e,4096);
        put(memory,preferred_e,3);
        require(sfr::NativeRenderer::guest_address(memory,physical,6)==preferred_e &&
                memory.load<uint16_t>(alias_a)==0,
                "new E mapping changes the actual caller source without writing old A");
        require(!fetch(renderer,memory,physical).buffer,
                "new preferred E source must not hit an A-only cached allocation or scan");
        work(renderer,0);
        const auto current=admit(renderer,memory,physical,3);
        work(renderer,1);
        paint(renderer,current,8,16);
        check(32,96);
    }

    {
        // Legacy vertices share the guarded cache, but their caller performs
        // the fill. Verify that the caller supplies the actual selected view.
        const auto input_vertex = compile(graphics, directory, header, R"(
struct Output { float4 position : SV_Position; float red : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID, float red : POSITION0) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position=float4(p[id],0.5,1); o.red=red; return o;
})", true);
        sfr::GuestMemory memory;
        sfr::NativeRenderer renderer(graphics,presentation);
        constexpr uint32_t physical=0x51000, alias_a=0xa0051000, preferred_e=0xe0050000;
        const auto put_vertices = [&](uint32_t address, float red) {
            for(uint32_t i=0;i<3;++i) memory.store<uint32_t>(address+i*4,std::bit_cast<uint32_t>(red));
        };
        uint32_t fills=0;
        const auto fetch_vertices = [&] {
            const auto address=sfr::NativeRenderer::guest_address(memory,physical,12);
            memory.check(address,12);
            auto cached=renderer.vertex_cache(memory,physical,memory.base()+address,12,12,4);
            if(!cached.fill.empty()) {
                require(cached.fill.size()==12,"legacy vertex replacement exposes the exact fill extent");
                sfr::swap_words_into(cached.fill,{memory.base()+address,12});
                ++fills;
            }
            return cached;
        };
        const auto admit_vertices = [&] {
            for(unsigned attempt=0;attempt<3;++attempt) {
                memory.advance_write_epoch();
                auto cached=fetch_vertices();
                if(cached.buffer) return cached;
            }
            throw std::runtime_error("legacy vertex source was not admitted after becoming stable");
        };
        memory.map(alias_a,4096); put_vertices(alias_a,0.125f);
        require(sfr::NativeRenderer::guest_address(memory,physical,12)==alias_a && !fetch_vertices().buffer,
                "legacy A-only vertex source first establishes watches");
        const auto old=admit_vertices();
        sfr::NativeDraw vertices;
        vertices.vertex_count=3; vertices.stride=4;
        vertices.vertex_shader=input_vertex.get(); vertices.pixel_shader=pixel.get();
        vertices.elements.emplace_back("POSITION",0,0,plume::RenderFormat::R32_FLOAT,0,0);
        vertices.vertex_buffer={old.buffer,old.offset};
        presentation.set_raster_state({0,0,16,16},{0,0,8,16});
        renderer.draw(vertices);
        memory.map(preferred_e,4096); put_vertices(preferred_e,0.375f);
        require(sfr::NativeRenderer::guest_address(memory,physical,12)==preferred_e && !fetch_vertices().buffer,
                "legacy cache rejects an A buffer when the caller changes to new preferred E");
        const auto current=admit_vertices();
        const auto hit=fetch_vertices();
        require(current.buffer!=old.buffer && hit.buffer==current.buffer && hit.fill.empty() && fills==2,
                "legacy source replacement fills once and subsequent draws reuse the new allocation");
        vertices.vertex_buffer={current.buffer,current.offset};
        presentation.set_raster_state({0,0,16,16},{8,0,16,16});
        renderer.draw(vertices);
        check(32,96);
    }

    // Use the real allocator in both retained nonheap and default heap modes.
    // Only one page of index data is touched; the heap's arena is not filled.
    for(const bool heap : {false,true}) {
        sfr::GuestMemory memory;
        sfr::PhysicalMemory allocator(memory,heap);
        sfr::NativeRenderer renderer(graphics,presentation);
        const auto allocate = [&] { return allocator.allocate(0,4096,4,0,0xffffffffu,4096); };
        const auto address=allocate();
        require(address!=0 && allocator.allocation_size(address)==4096,
                "real physical allocator provides one page for the index fixture");
        const auto physical=uint32_t(uint64_t(address)-sfr::PhysicalMemory::arena_base+sfr::PhysicalMemory::physical_offset);
        // Mark before publication: marking itself checks writes, while free of
        // retained GPU backing alone is not a content mutation.
        allocator.mark_gpu_backed(address,4096);
        put(memory,address,0);
        require(!fetch(renderer,memory,physical).buffer,"allocated source first establishes watches");
        auto old=admit(renderer,memory,physical,0);
        work(renderer,1);
        paint(renderer,old,0,8);
        require(allocator.free(address) && allocator.allocation_size(address)==0 && memory.readable(address,6),
                "heap or GPU-retained free relinquishes ownership while retaining backing");
        require(fetch(renderer,memory,physical).buffer==old.buffer,
                "unchanged retained content may keep its cache after allocator free");
        work(renderer,0);
        const auto reused=allocate();
        require(reused==address && memory.load<uint32_t>(reused)==0,
                "real allocator reuses and zeroes the same guest address");
        require(!fetch(renderer,memory,physical).buffer,
                "allocator zeroing invalidates the old index cache before new content is written");
        put(memory,reused,3);
        auto current=admit(renderer,memory,physical,3);
        work(renderer,1);
        paint(renderer,current,8,16);
        check(32,96);

        if(!heap) {
            // Reuse creates an ordinary allocation again. Its free really
            // releases backing; the next allocation maps the same address.
            paint(renderer,current,0,8);
            require(allocator.free(reused) && !memory.readable(reused,6),
                    "ordinary physical free releases its GuestMemory backing");
            const auto remapped=allocate();
            require(remapped==address && memory.load<uint32_t>(remapped)==0,
                    "ordinary reallocation remaps and zeroes the old address");
            put(memory,remapped,6);
            require(!fetch(renderer,memory,physical).buffer,"release and remap invalidate the previous cached bytes");
            current=admit(renderer,memory,physical,6);
            work(renderer,1);
            paint(renderer,current,8,16);
            check(96,159);

            paint(renderer,current,0,8);
            memory.decommit(remapped,4096);
            require(!memory.readable(remapped,6),"decommit removes the live source mapping");
            memory.commit(remapped,4096);
            require(memory.load<uint32_t>(remapped)==0,"recommit restores zeroed backing at the same address");
            put(memory,remapped,9);
            require(!fetch(renderer,memory,physical).buffer,"decommit and recommit invalidate the previous index scan");
            current=admit(renderer,memory,physical,9);
            work(renderer,1);
            paint(renderer,current,8,16);
            check(159,223);
        }
        require(allocator.free(address),"physical lifecycle fixture releases its final live allocation");
    }
}

void prepared_dec3n_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
struct Output { float4 position : SV_Position; float4 color : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID, float4 normal : POSITION0) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position=float4(p[id],0.5,1);
 o.color=float4(normal.xyz*0.5+0.5,normal.w); return o;
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain(float4 position : SV_Position, float4 color : TEXCOORD0) : SV_Target { return color; }
)", false);
    environment("SFR_RENDER_SCALE", "100");
    sfr::GuestMemory memory;
    sfr::GuestBufferState state(memory);
    constexpr uint32_t resource=0x10000, backing=0xe0010000, physical=0x11000;
    constexpr uint32_t stride=8, expanded_stride=16, resource_bytes=56;
    constexpr std::array<uint32_t,1> offsets{4};
    memory.map(resource,4096); memory.map(backing,4096);
    const auto pack=[](int x,int y,int z) {
        return (uint32_t(x)&1023u) | ((uint32_t(y)&1023u)<<10) | ((uint32_t(z)&1023u)<<20);
    };
    const uint32_t red=pack(511,-512,0), green=pack(0,511,-511), blue=pack(-511,0,511);
    const auto create=[&] {
        memory.store<uint32_t>(resource,0x100001);
        memory.store<uint32_t>(resource+24,backing|3);
        memory.store<uint32_t>(resource+28,resource_bytes|0x10000002);
        state.created(resource);
    };
    const auto write_vertices=[&](uint32_t second_color) {
        // Four bytes before and after the six records deliberately put their
        // phase off the resource base. The requested draw starts at record 3.
        for(uint32_t i=0;i<6;++i) {
            memory.store<uint32_t>(backing+4+i*stride,0x3f000000);
            memory.store<uint32_t>(backing+8+i*stride,i<3?blue:second_color);
        }
    };
    const auto view=[&] {
        auto result=state.vertices(resource,physical+28,3*stride,stride,offsets,true);
        require(bool(result.snapshot) && result.offset==3*expanded_stride,
                "prepared DEC3N view uses expanded subrange offsets after a partial-record prefix");
        return result;
    };
    const auto update=[&](uint32_t color) {
        state.lock(resource); write_vertices(color); state.unlock(resource,0x100);
        return view();
    };
    sfr::NativePresentation presentation(graphics,16,16);
    sfr::NativeRenderer renderer(graphics,presentation);
    sfr::NativeDraw draw;
    draw.vertex_count=3; draw.stride=expanded_stride;
    draw.vertex_shader=vertex.get(); draw.pixel_shader=pixel.get();
    draw.elements.emplace_back("POSITION",0,0,plume::RenderFormat::R16G16B16A16_SNORM,0,stride);
    const auto bind=[&](const sfr::GuestVertexView& prepared) {
        draw.vertices={}; draw.vertex_buffer={}; draw.vertex_snapshot.reset();
        draw.vertex_snapshot_offset=0;
        if(prepared.snapshot->transient) {
            draw.vertex_snapshot=prepared.snapshot;
            draw.vertex_snapshot_offset=prepared.offset;
        } else {
            draw.vertex_buffer={renderer.managed_vertex_buffer(prepared.snapshot),prepared.offset};
            require(draw.vertex_buffer.buffer!=nullptr,"prepared static DEC3N data owns a GPU buffer");
        }
    };
    const auto column=[&](int x) {
        presentation.set_raster_state({0,0,16,16},{x*4,0,(x+1)*4,16});
        renderer.draw(draw);
    };
    const auto check=[&](const std::array<std::array<int,4>,4>& colors) {
        const auto pixels=presentation.readback_color();
        for(uint32_t y=0;y<16;++y) for(uint32_t x=0;x<16;++x) for(uint32_t c=0;c<4;++c)
            require(std::abs(int(pixels[(y*16+x)*4+c])-colors[x/4][c])<=1,
                    "GPU decodes each immutable DEC3N version at its expanded subrange offset");
    };
    // Expected BGRA comes from the endpoints -1, 0 and +1, independently of
    // the production conversion code. -512 must clamp to the same -1 as -511.
    constexpr std::array<int,4> red_pixel{128,0,255,255}, green_pixel{0,255,128,255}, blue_pixel{255,128,0,255};
    create(); auto old=update(red);
    const auto old_version=old.snapshot->version;
    bind(old); const auto old_binding=draw.vertex_buffer; column(0);
    auto newer=update(green);
    require(newer.snapshot->version!=old_version,"Unlock replaces a prepared DEC3N version");
    bind(newer); column(1);
    (void)renderer.take_pipeline_work();
    bind(old); bind(old);
    const auto restored_static=renderer.take_pipeline_work();
    require(restored_static.managed_vertex_lookups==1 && restored_static.managed_vertex_allocations==0,
            "returning to an older static version finds its owned buffer once, then memoizes it");
    state.released(resource,state.identity(resource)); old.snapshot.reset(); newer.snapshot.reset();
    // Switch the reused NativeDraw from a nonzero managed offset to ordinary
    // host-order upload data, then back to the queued, already released buffer.
    std::array<uint8_t,3*expanded_stride> ordinary{};
    const std::array<int16_t,4> ordinary_normal{-32767,0,32767,32767};
    for(uint32_t i=0;i<3;++i)
        std::memcpy(ordinary.data()+i*expanded_stride+stride,ordinary_normal.data(),sizeof(ordinary_normal));
    draw.vertex_buffer={}; draw.vertices=ordinary; column(2);
    draw.vertices={}; draw.vertex_buffer=old_binding; column(3);
    check({red_pixel,green_pixel,blue_pixel,red_pixel});

    create();
    const auto dynamic_update=[&](uint32_t color) {
        memory.store<uint32_t>(resource,0x100101);
        state.begin_range(resource,physical+4,6*stride);
        write_vertices(color);
        memory.store<uint32_t>(resource,0x100001);
        state.finish_ranges(resource,0x100);
        auto result=view();
        require(result.snapshot->transient,"prepared dynamic DEC3N data retains draw-ring ownership");
        return result;
    };
    auto dynamic=dynamic_update(red);
    auto repeated=view();
    require(repeated.snapshot==dynamic.snapshot && repeated.offset==dynamic.offset,
            "repeated DEC3N draws share one prepared dynamic version");
    repeated.snapshot.reset();
    bind(dynamic); (void)renderer.take_pipeline_work();
    column(0); column(1);
    const auto previous_version=dynamic.snapshot->version;
    dynamic=dynamic_update(green);
    require(dynamic.snapshot->version!=previous_version,"dynamic wrap publishes a new prepared version");
    bind(dynamic); column(2); column(3);
    const auto dynamic_work=renderer.take_pipeline_work();
    require(dynamic_work.draw_ring_bytes==4*(8192+768)+2*256 && dynamic_work.transient_vertex_lookups==2,
            "each prepared dynamic DEC3N version uploads once per ring despite repeated draws");
    state.released(resource,state.identity(resource)); dynamic.snapshot.reset();
    check({red_pixel,red_pixel,green_pixel,green_pixel});
    // A released immutable view remains valid across asynchronous ring reuse;
    // each new ring must upload its expanded data before reusing the offset.
    for(int frame=0;frame<3;++frame) {
        presentation.set_raster_state({0,0,16,16},{0,0,16,16});
        renderer.draw(draw); renderer.draw(draw);
        const auto work=renderer.take_pipeline_work();
        require(work.draw_ring_bytes==2*(8192+768)+256 && work.transient_vertex_lookups==1,
                "new rings reupload prepared DEC3N slices once and retain their expanded offset");
        presentation.present();
    }
    check({green_pixel,green_pixel,green_pixel,green_pixel});
    // Fill most of a fresh ring in place, without a large CPU scratch copy.
    // Two dynamic draws fit after it; the third starts with a memo hit but
    // must flush inside draw(), then resolve/upload again at a fresh offset.
    auto filler=draw;
    filler.vertex_snapshot.reset(); filler.vertex_snapshot_offset=0;
    auto filler_space=renderer.vertex_space((64ull<<20)-32768,0);
    require(!filler_space.empty(),"prepared slice capacity fixture reserves a fresh ring");
    std::memcpy(filler_space.data(),ordinary.data(),ordinary.size());
    filler.vertices=filler_space;
    presentation.set_raster_state({0,0,16,16},{0,0,4,16});
    renderer.draw(filler);
    (void)renderer.take_pipeline_work();
    column(1); column(2); column(3);
    const auto capacity_work=renderer.take_pipeline_work();
    require(capacity_work.ring_flushes==1 && capacity_work.transient_vertex_lookups==2 &&
            capacity_work.draw_ring_bytes==3*(8192+768)+2*256,
            "draw-time capacity flush discards a dynamic memo hit and recomputes its allocation");
    check({blue_pixel,green_pixel,green_pixel,green_pixel});
    std::cerr << "PREPARED_DEC3N_LIFETIME static/dynamic/subrange/reuse/release passed\n";
}

void constant_upload_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
#ifndef __spirv__
cbuffer VSConstants : register(b0, space4) { float4 vsConstants[256]; };
#endif
struct Output { float4 position : SV_Position; float2 red_blue : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position = float4(p[id],0.5,1);
#ifdef __spirv__
 o.red_blue = float2(asfloat(vk::RawBufferLoad<uint>(g_PushConstants.VertexShaderConstants + 4092)),
                    asfloat(vk::RawBufferLoad<uint>(g_PushConstants.VertexShaderConstants + 2052)));
#else
 o.red_blue = float2(vsConstants[255].w,vsConstants[128].y);
#endif
 return o;
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
#ifndef __spirv__
cbuffer PSConstants : register(b1, space4) { float4 psConstants[256]; };
#endif
// Retain the VS signature order for D3D12 linkage even though position is unused.
float4 shaderMain(float4 position : SV_Position, float2 red_blue : TEXCOORD0) : SV_Target {
#ifdef __spirv__
 float green = asfloat(vk::RawBufferLoad<uint>(g_PushConstants.PixelShaderConstants + 0));
#else
 float green = psConstants[0].x;
#endif
 return float4(red_blue.x,green,red_blue.y,1);
})", false);
    environment("SFR_RENDER_SCALE", "100");
    for (int mode : {0, 1, 2, 3}) {
        const bool reuse = mode != 0, managed = mode >= 2, direct = mode == 3;
        std::cerr << "CONSTANT_READBACK reuse=" << reuse << " managed=" << managed << " direct=" << direct << " begin\n";
        // Managed versions take precedence even if the older comparison cache
        // is requested too; only TRACE intentionally materializes diagnostic arrays.
        environment("SFR_CONSTANT_UPLOAD_REUSE", mode == 1 || direct ? "1" : "0");
        environment("SFR_DIRECT_CONSTANT_UPLOAD", direct ? "1" : "0");
        unsigned asynchronous_flushes = 0;
        sfr::NativePresentation presentation(graphics, 16, 16);
        sfr::NativeRenderer renderer(graphics, presentation);
        presentation.after_flush([&](bool complete) { if (!complete) ++asynchronous_flushes; });
        sfr::NativeClear clear{};
        clear.color = true;
        presentation.clear(clear);
        sfr::NativeDraw draw;
        draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
        sfr::GuestConstantState::Values vs_values{}, ps_values{};
        vs_values[513] = std::bit_cast<uint32_t>(0.25f); // unchanged gap between sparse dirty groups
        std::array<uint8_t,4096> vs_source{}, ps_source{};
        sfr::GuestConstantState vs_state, ps_state;
        draw.vertex_constants = vs_values; draw.pixel_constants = ps_values;
        const auto encode = [](const sfr::GuestConstantState::Values& values, std::array<uint8_t,4096>& big_endian) {
            for (size_t i=0; i<values.size(); ++i) {
                const auto value=__builtin_bswap32(values[i]);
                std::memcpy(big_endian.data()+4*i,&value,4);
            }
        };
        const auto prepare = [&](sfr::GuestConstantState& state, const sfr::GuestConstantState::Values& values,
                                 std::array<uint8_t,4096>& source, uint64_t dirty) {
            encode(values,source); state.invalidate_mask(dirty);
            if (direct) state.publish_version(); // no CPU shadow conversion before the direct upload
            else state.prepare(source);
        };
        const std::array<float,4> red{0.25f,0.25f,0.75f,0.75f}, green{0.5f,0.5f,0.5f,0.875f};
        for (int column = 0; column < 4; ++column) {
            vs_values.back() = std::bit_cast<uint32_t>(red[column]);
            ps_values.front() = std::bit_cast<uint32_t>(green[column]);
            if (managed) {
                if (!column || red[column] != red[column-1]) prepare(vs_state,vs_values,vs_source,column ? 1ull : ~0ull);
                if (!column || green[column] != green[column-1]) prepare(ps_state,ps_values,ps_source,column ? 1ull<<63 : ~0ull);
                draw.vertex_constant_state = &vs_state; draw.pixel_constant_state = &ps_state;
                draw.vertex_constants = vs_state.values(); draw.pixel_constants = ps_state.values();
                if (direct) {
                    draw.vertex_constants_source = vs_source; draw.pixel_constants_source = ps_source;
                    require(vs_state.values()[513] == 0 && ps_state.values().front() == 0,
                            "direct fixture must not accidentally prepare the CPU shadow");
                }
            }
            presentation.set_raster_state({0,0,16,16}, {column*4,0,(column+1)*4,16});
            renderer.draw(draw);
            if (direct) {
                // Every queued draw owns its converted bytes before return.
                // A following clean stage must also ignore this poisoned source,
                // including when the other stage has a new sparse dirty version.
                vs_source.fill(0xff); ps_source.fill(0xff);
            }
        }
        const auto columns_work = renderer.take_pipeline_work();
        require(columns_work.constant_saved_bytes == (reuse ? 16384 : 0),
                "identical VS/PS stages skip writes independently, only when enabled");
        require(columns_work.draw_ring_bytes == 4 * (8192 + 768) - (managed ? 16384 : 0),
                "managed reuse also eliminates unused stage allocations, with aligned remaining data");
        require(columns_work.constant_written_bytes == (reuse ? 16384 : 32768) &&
                columns_work.direct_constant_read_bytes == (direct ? 16384 : 0),
                "direct source reads occur only for changed stages and fill complete immutable uploads");
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
        // The next ring needs the current complete guest image again, even
        // though owner/version are unchanged and the CPU shadow remains empty.
        if (direct) { encode(vs_values,vs_source); encode(ps_values,ps_source); }
        const auto expect_upload = [&](uint64_t saved, const char* message) {
            const auto work = renderer.take_pipeline_work();
            require(work.constant_saved_bytes == saved && work.constant_written_bytes == 8192-saved &&
                    work.direct_constant_read_bytes == (direct ? 8192-saved : 0), message);
        };
        // readback flushes and recycles the ring. Identical values must upload
        // again; merely finding old bytes still in mapped storage is not enough.
        presentation.set_raster_state({0,0,16,16}, {0,0,16,16});
        renderer.draw(draw);
        expect_upload(0, "flush invalidates both stage offsets and reloads direct sources");
        renderer.draw(draw);
        expect_upload(reuse ? 8192 : 0, "reuse resumes without source reads after the fresh upload");
        const auto repeated = presentation.readback_color();
        std::cerr << "CONSTANT_READBACK recycled ring read\n";
        for (size_t at=0; at<repeated.size(); at+=4)
            require(std::abs(int(repeated[at+2])-191)<=1 && std::abs(int(repeated[at+1])-223)<=1 && std::abs(int(repeated[at])-64)<=1,
                    "recycled ring produces fresh constants rather than stale addresses");
        // Rotate through both rings while previous GPU submissions can remain
        // in flight. Every fresh frame must upload before it can reuse again.
        for (int frame=0; frame<4; ++frame) {
            renderer.draw(draw);
            expect_upload(0, "asynchronous ring switch invalidates both stages");
            renderer.draw(draw);
            expect_upload(reuse ? 8192 : 0, "new ring reuses only its own upload");
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
        expect_upload(0, "capacity flush invalidates cached offsets and reloads complete direct sources");
        const auto final = presentation.readback_color();
        for (size_t at=0; at<final.size(); at+=4)
            require(std::abs(int(final[at+2])-191)<=1 && std::abs(int(final[at+1])-223)<=1 && std::abs(int(final[at])-64)<=1,
                    "ring switches and capacity flush preserve rendered constants");
    }
    environment("SFR_CONSTANT_UPLOAD_REUSE", "0");
    environment("SFR_DIRECT_CONSTANT_UPLOAD", "0");
}
void short_constant_upload_readback(sfr::NativeGraphics& graphics, const fs::path& directory, const std::string& header) {
    const auto vertex = compile(graphics, directory, header, R"(
cbuffer LoopConstants : register(b4, space4) { int4 g_LoopConstants[16]; };
struct Output { float4 position : SV_Position; float2 loops : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position = float4(p[id],0.5,1);
 o.loops = (float2(g_LoopConstants[0].x,g_LoopConstants[15].w)+128.0)/255.0;
 return o;
})", true);
    const auto pixel = compile(graphics, directory, header, R"(
float4 shaderMain(float4 position : SV_Position, float2 loops : TEXCOORD0) : SV_Target {
 uint ends = (g_Booleans & 1) | (((g_Booleans >> 15) & 1) << 1) |
             (((g_Booleans >> 16) & 1) << 2) | (((g_Booleans >> 31) & 1) << 3);
 return float4(loops,float(ends)/15.0,1);
})", false);
    environment("SFR_RENDER_SCALE", "100");
    environment("SFR_CONSTANT_UPLOAD_REUSE", "0");
    unsigned asynchronous_flushes = 0;
    sfr::NativePresentation presentation(graphics, 24, 8);
    sfr::NativeRenderer renderer(graphics, presentation);
    presentation.after_flush([&](bool complete) { if (!complete) ++asynchronous_flushes; });
    sfr::NativeClear clear{}; clear.color = true; presentation.clear(clear);
    sfr::NativeDraw draw;
    draw.vertex_count = 3; draw.vertex_shader = vertex.get(); draw.pixel_shader = pixel.get();
    sfr::GuestShortConstantState state, other;
    const auto prepare = [](sfr::GuestShortConstantState& target, int first, int last, uint32_t booleans) {
        std::array<uint8_t, sfr::GuestShortConstantState::source_bytes> source{};
        const auto word = [&](size_t offset, uint32_t value) {
            value = __builtin_bswap32(value); std::memcpy(source.data()+offset, &value, 4);
        };
        // The two low 16-bit bool groups and the opposite ends of i0/i15.
        // Unused high bits must not leak into the shader's boolean mask.
        word(0, 0xa5a50000u | (booleans & 0xffffu));
        word(16, 0x5a5a0000u | (booleans >> 16));
        source[35] = uint8_t(first); source[92] = uint8_t(last);
        target.invalidate(); target.prepare(source);
    };
    const auto column = [&](int x, const sfr::GuestShortConstantState* owner, uint32_t legacy_booleans = 0) {
        draw.short_constant_state = owner;
        draw.shared.booleans = owner ? owner->booleans() : legacy_booleans;
        presentation.set_raster_state({0,0,24,8}, {x*4,0,(x+1)*4,8});
        renderer.draw(draw);
    };
    prepare(state, -128, 127, 0x80018001u);
    column(0, &state); column(1, &state);
    prepare(state, 127, -128, 0x00018000u);
    column(2, &state);  // Earlier queued columns must retain the old version.
    draw.loop_constants.front() = draw.loop_constants.back() = -128;
    column(3, nullptr, 0x80000001u);
    column(4, &state);  // Legacy uploads must not replace the managed offset.
    other.invalidate(); prepare(other, 0, 127, 0x80008000u);
    require(other.version() == state.version(), "owner transition tests equal numeric versions");
    column(5, &other);
    const auto work = renderer.take_pipeline_work();
    require(work.loop_constant_written_bytes == 4*256 && work.loop_constant_saved_bytes == 2*256,
            "loop uploads distinguish dirty versions and owners while reusing unchanged offsets");
    require(work.draw_ring_bytes == 6*(8192+768)-2*256, "loop reuse also removes its ring allocation");
    const auto check = [](const std::vector<uint8_t>& pixels, bool columns) {
        // Independent RGBA oracle for signed endpoints and four bool ends.
        const std::array<std::array<int,4>,6> expected{{
            {0,255,255,255}, {0,255,255,255}, {255,0,102,255},
            {0,0,153,255}, {255,0,102,255}, {128,255,170,255}}};
        require(pixels.size() == 24*8*4, "short constant readback has the expected extent");
        for (size_t at=0; at<pixels.size(); at+=4) {
            const auto& color = expected[columns ? (at/4%24)/4 : 5];
            require(std::abs(int(pixels[at+2])-color[0])<=1 && std::abs(int(pixels[at+1])-color[1])<=1 &&
                    std::abs(int(pixels[at])-color[2])<=1 && pixels[at+3]==color[3],
                    "GPU loop offsets and bool snapshots preserve each queued draw's values");
        }
    };
    check(presentation.readback_color(), true);
    presentation.set_raster_state({0,0,24,8}, {0,0,24,8});
    const auto draw_checked = [&](bool reused) {
        renderer.draw(draw);
        const auto counters = renderer.take_pipeline_work();
        require(counters.loop_constant_written_bytes == (reused ? 0 : 256) &&
                counters.loop_constant_saved_bytes == (reused ? 256 : 0),
                "ring lifetime requires a fresh loop upload before reuse");
    };
    draw_checked(false); draw_checked(true);
    check(presentation.readback_color(), false);
    for (int frame=0; frame<4; ++frame) {
        draw_checked(false); draw_checked(true);
        presentation.present();
    }
    require(asynchronous_flushes >= 4, "loop test exercises asynchronous ring rotation");
    draw_checked(false); draw_checked(true);
    require(!renderer.vertex_space((64ull << 20)-32768, 0).empty(), "loop capacity reservation fits a fresh ring");
    require(renderer.take_pipeline_work().ring_flushes == 1, "loop test exercises capacity flush");
    draw_checked(false);
    check(presentation.readback_color(), false);

    // This second oracle starts with original Set*ShaderConstantI opcodes.
    // Unlike embedded packed values above, the setter packs x/y/z and clears w.
    const auto original_vertex = compile(graphics, directory, header, R"(
cbuffer LoopConstants : register(b4, space4) { int4 g_LoopConstants[16]; };
struct Output { float4 position : SV_Position; float2 loops : TEXCOORD0; };
Output shaderMain(uint id : SV_VertexID) {
 float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
 Output o; o.position = float4(p[id],0.5,1);
 o.loops = g_LoopConstants[0].w==0 && g_LoopConstants[15].w==0 ?
     (float2(g_LoopConstants[0].x,g_LoopConstants[15].z)+128.0)/255.0 : float2(0.25,0.25);
 return o;
})", true);
    sfr::GuestMemory memory;
    constexpr uint32_t device=0x10000000, input=0x10010000;
    memory.map(device,0x4000); memory.map(input,0x1000);
    draw.vertex_shader=original_vertex.get(); draw.shared.booleans=0x80018001;
    for (bool managed : {false,true}) {
        sfr::GuestShortConstantState original_state;
        memory.store<uint32_t>(device+10112,0x8001);
        memory.store<uint32_t>(device+10128,0x8001);
        memory.store<uint32_t>(device+10140,0x55667744); // padding BEFORE VS i0
        for (uint32_t i=0; i<16; ++i) {
            memory.store<uint32_t>(input+i*16,i ? 1 : uint32_t(-128));
            memory.store<uint32_t>(input+i*16+4,2);
            memory.store<uint32_t>(input+i*16+8,i==15 ? 127 : 3);
            memory.store<uint32_t>(input+i*16+12,0x7f); // original setter ignores w
        }
        memory.store<uint32_t>(input+256,0xef); memory.store<uint32_t>(input+260,0xcd);
        memory.store<uint32_t>(input+264,0xab); memory.store<uint32_t>(input+268,0x7f);
        sfr::test::original_loop_setter(memory,true,device,0,input+256,1);
        sfr::test::original_loop_setter(memory,false,device,0,input,16);
        require(memory.load<uint32_t>(device+10144)==0x00030280 &&
                memory.load<uint32_t>(device+10204)==0x007f0201,
                "original update-store places VS i0 and i15 at their actual device offsets");
        const auto paint_original = [&](int left, int right) {
            require(memory.load<uint32_t>(device+10140)==0x55667744 &&
                    memory.load<uint32_t>(device+10208)==0x00abcdef,
                    "original VS setters preserve the preceding padding and following PS i0");
            if (managed) {
                original_state.invalidate_mask(memory.load<uint64_t>(device+32));
                original_state.prepare(std::span<const uint8_t,sfr::GuestShortConstantState::source_bytes>(
                    memory.base()+device+10112,sfr::GuestShortConstantState::source_bytes));
                draw.short_constant_state=&original_state;
            } else {
                draw.short_constant_state=nullptr;
                for (uint32_t i=0; i<16; ++i) {
                    const uint32_t packed=memory.load<uint32_t>(device+10144+4*i);
                    for (uint32_t lane=0; lane<4; ++lane)
                        draw.loop_constants[i*4+lane]=int32_t(int8_t(packed>>(lane*8)));
                }
            }
            memory.store<uint64_t>(device+32,0);
            presentation.set_raster_state({0,0,24,8},{left,0,right,8}); renderer.draw(draw);
        };
        presentation.clear(clear); paint_original(0,12);
        memory.store<uint32_t>(input,127); memory.store<uint32_t>(input+15*16+8,uint32_t(-128));
        sfr::test::original_loop_setter(memory,false,device,0,input,1);
        sfr::test::original_loop_setter(memory,false,device,15,input+15*16,1);
        paint_original(12,24);
        const auto pixels=presentation.readback_color();
        require(pixels.size()==24*8*4, "original setter readback has its requested extent");
        for (size_t at=0; at<pixels.size(); at+=4) {
            const bool old=(at/4%24)<12;
            require(pixels[at+2]==(old ? 0 : 255) && pixels[at+1]==(old ? 255 : 0) &&
                    pixels[at]==255 && pixels[at+3]==255,
                    "original setters reach GPU i0/i15 without padding/PS contamination or queued-version corruption");
        }
    }
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
        sfr::GuestConstantState::Values vs_values{}, ps_values{};
        draw.vertex_constants = vs_values; draw.pixel_constants = ps_values;
        const auto paint = [&](float z, std::array<float, 4> color) {
            vs_values[0] = std::bit_cast<uint32_t>(z);
            for (size_t i=0; i<color.size(); ++i)
                ps_values[i] = std::bit_cast<uint32_t>(color[i]);
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
    // Exercise the real prepared GuestGraphics state and NativeRenderer queue,
    // rather than supplying hand-decoded stencil descriptors as above. The
    // original-opcode fixture invokes the same post-setter notification API;
    // actual hook routing is separately source-audited and sampled in gameplay.
    environment("SFR_RENDER_SCALE", "100");
    sfr::GuestMemory memory;
    constexpr uint32_t params=0x10000000, output=params+124;
    constexpr uint32_t device=sfr::GuestGraphics::device_address;
    memory.map(params,0x1000); memory.map(0x82AD0000,0x2000);
    constexpr std::array<uint32_t,31> parameters{
        16,16,0x18280186,1,0,0,1,0,0,1,0x1A220197,0,0,1,0,0,0x28280106,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    for (size_t i=0; i<parameters.size(); ++i) memory.store<uint32_t>(params+i*4,parameters[i]);
    for (auto [table,count] : {std::pair{0x82AD0A60u,101u},std::pair{0x82AD0F20u,20u}})
        for (uint32_t i=0; i<count; ++i) {
            memory.store<uint32_t>(table+i*12,0x12340000+i);
            memory.store<uint32_t>(table+i*12+4,0x82210000+i*4);
            memory.store<uint32_t>(table+i*12+8,i);
        }
    sfr::test::install_blend_defaults(memory); sfr::test::install_sampler_defaults(memory);
    sfr::GuestGraphics guest(memory,graphics);
    require(guest.create_device(0,1,0,0,params,output)==0,"stencil fixture creates the actual guest device");
    auto& presentation=guest.presentation(); auto& renderer=guest.renderer();
    memory.store<uint32_t>(device+10548,0);
    memory.store<uint32_t>(device+10496,0x00FFFF00);
    memory.store<uint32_t>(device+12632,sfr::GuestGraphics::depth_handle);
    sfr::NativeDraw draw;
    const std::array<uint8_t,12> vertices{};
    const std::array<uint32_t,3> reverse{0,2,1};
    sfr::GuestConstantState::Values vs_values{},ps_values{};
    draw.vertex_count=3; draw.stride=4; draw.vertices=vertices;
    draw.vertex_shader=vertex.get(); draw.pixel_shader=pixel.get();
    draw.vertex_constants=vs_values; draw.pixel_constants=ps_values;
    const auto set=[&](uint32_t entry,uint32_t value) {
        sfr::test::original_stencil_setter(memory,entry,device,value);
        guest.invalidate_stencil_state(entry>=0x824E7390 ? sfr::GuestGraphics::stencil_refmask_pending :
                                                          sfr::GuestGraphics::stencil_control_pending);
    };
    const auto paint=[&](int left,int right,std::array<float,4> color,bool backwards=false) {
        const auto bytes=guest.prepare_stencil_state(true);
        const auto& state=guest.stencil_state();
        // Attachment selection is live at consume time, even with a clean raw
        // snapshot; none of these fields is borrowed by the queued draw.
        draw.stencil_enabled=state.enabled && memory.load<uint32_t>(device+12632)!=0;
        draw.stencil_reference=state.reference;
        draw.stencil_read_mask=state.read_mask; draw.stencil_write_mask=state.write_mask;
        draw.stencil_front=state.front; draw.stencil_back=state.back;
        draw.indices=backwards ? std::span<const uint32_t>(reverse) : std::span<const uint32_t>{};
        for (size_t i=0; i<color.size(); ++i) ps_values[i]=std::bit_cast<uint32_t>(color[i]);
        presentation.set_raster_state({0,0,16,16},{left,0,right,16});
        renderer.draw(draw);
        return bytes;
    };
    const auto check=[&](const char* stage,std::array<uint8_t,4> left,std::array<uint8_t,4> right) {
        const auto pixels=presentation.readback_color();
        require(pixels.size()==16*16*4,"prepared stencil readback has the requested extent");
        for (uint32_t y=0; y<16; ++y) for (uint32_t x=0; x<16; ++x) {
            const auto& expected=x<8 ? left : right;
            const auto at=(y*16+x)*4;
            for (size_t channel=0; channel<4; ++channel) if (pixels[at+channel]!=expected[channel]) {
                const auto& state=guest.stencil_state();
                throw std::runtime_error(std::string("prepared stencil stage=")+stage+
                    " x="+std::to_string(x)+" y="+std::to_string(y)+
                    " actual_bgra="+std::to_string(pixels[at])+","+std::to_string(pixels[at+1])+","+
                    std::to_string(pixels[at+2])+","+std::to_string(pixels[at+3])+
                    " expected_bgra="+std::to_string(expected[0])+","+std::to_string(expected[1])+","+
                    std::to_string(expected[2])+","+std::to_string(expected[3])+
                    " current_control="+std::to_string(state.control)+" current_refmask="+std::to_string(state.refmask));
            }
        }
    };
    sfr::NativeClear clear{};
    clear.color=clear.depth=clear.stencil=true; clear.color_value={0,0,0,1};
    presentation.clear(clear);
    set(0x824E7178,1); // stencil enable, with a real attached depth target
    set(0x824E71F0,7); // front compare ALWAYS; back mirrors front
    set(0x824E7290,2); // pass REPLACE
    set(0x824E7390,0x123400A5); set(0x824E73D0,0x0F);
    require(paint(0,8,{1,0,0,1})==8,"first prepared stencil draw consumes both words");
    set(0x824E7390,0xBC); set(0x824E73D0,0xF0);
    require(paint(8,16,{0,1,0,1})==4,"second queued draw consumes only changed reference/write mask");
    // Left stencil is 0x05 and right is 0xB0. Consume before a readback flush
    // so a mutable/borrowed cached descriptor would corrupt the older draws.
    set(0x824E71F0,2); set(0x824E7290,0); // EQUAL, KEEP
    set(0x824E73B0,0x0F); set(0x824E7390,0x15);
    require(paint(0,16,{0,0,1,1})==8,"comparison and reference changes coalesce independently");
    check("low-read-mask",{255,0,0,255},{0,255,0,255});
    set(0x824E73B0,0xF0); set(0x824E7390,0xB7);
    require(paint(0,16,{1,1,0,1})==4,"read mask gates the independent reference byte");
    check("high-read-mask",{255,0,0,255},{0,255,255,255});

    set(0x824E71F0,0); // NEVER
    require(guest.prepare_stencil_state(true)==4 && guest.stencil_state().enabled,
            "raw stencil enable is prepared without baking in effective attachment state");
    // Keep the raw register untouched to isolate live attachment gating. The
    // original SetDepthStencilSurface hook's extra writes are audited elsewhere.
    memory.store<uint32_t>(device+12632,0);
    require(paint(0,16,{1,1,1,1})==0 && guest.stencil_state().enabled,
            "unbinding disables the clean prepared stencil test only for this draw");
    memory.store<uint32_t>(device+12632,sfr::GuestGraphics::depth_handle);
    require(paint(0,16,{1,0,0,1})==0,
            "reattachment re-enables the same raw NEVER test without a source refresh");
    check("attachment-rebind",{255,255,255,255},{255,255,255,255});

    set(0x824E71B8,1); set(0x824E71F0,7); set(0x824E72C0,0);
    set(0x824E73D0,0); // face selection affects color, leaving stencil intact
    paint(0,8,{1,0,0,1}); paint(8,16,{0,1,0,1},true);
    const auto faces=presentation.readback_color();
    // The CPU opcode oracle fixes which register is front/back. Here test the
    // two actual windings as a pair without assuming a backend's viewport Y
    // convention: exactly one winding passes, and swapping the face compares
    // must invert that choice.
    const bool left_passed=faces[0]==0 && faces[1]==0 && faces[2]==255;
    if (left_passed) check("opposite-faces-left-passed",{0,0,255,255},{255,255,255,255});
    else check("opposite-faces-right-passed",{255,255,255,255},{0,255,0,255});
    // Readback flushed the list and reset the same ring: repeating nonindexed
    // then indexed draws reuses the IB offset, but must rebind it on the new list.
    clear.depth=clear.stencil=false; clear.color_value={1,1,1,1}; presentation.clear(clear);
    set(0x824E71F0,0); set(0x824E72C0,7);
    paint(0,8,{0,0,1,1}); paint(8,16,{1,1,0,1},true);
    if (left_passed) check("swapped-faces-right-passed",{255,255,255,255},{0,255,255,255});
    else check("swapped-faces-left-passed",{255,0,0,255},{255,255,255,255});
    // Keep every transition queued until readback. The middle two stripes
    // contain stencil 1 and the outer stripes 0, making a missing reference
    // update visible independently in each stripe.
    clear.color=clear.depth=clear.stencil=true; clear.color_value={0,0,0,1};
    presentation.clear(clear);
    set(0x824E71B8,0); set(0x824E7178,1);
    set(0x824E71F0,7); set(0x824E7290,2);
    set(0x824E73B0,0xFF); set(0x824E73D0,0xFF); set(0x824E7390,1);
    draw.write_mask=0;
    paint(4,12,{0,0,0,1});
    set(0x824E71F0,2); set(0x824E7290,0); // EQUAL, KEEP
    draw.write_mask=0xF;
    set(0x824E7390,0); paint(0,4,{1,0,0,1});
    set(0x824E7390,1); paint(4,8,{0,1,0,1},true);
    draw.write_mask=0xD; // different pipeline, same reference; blue remains writable
    paint(8,12,{0,0,1,1});
    set(0x824E7178,0); set(0x824E7390,99);
    draw.write_mask=0;
    paint(4,8,{0,0,0,1},true); // interleave a disabled pipeline without changing the stripe oracle
    set(0x824E7178,1); set(0x824E7390,1);
    draw.write_mask=0xF;
    paint(4,8,{1,1,0,1},true); // re-enable reference 1 after the disabled pipeline
    set(0x824E7390,0); paint(12,16,{1,1,1,1},true);
    const auto check_stripes=[&](const char* stage,const std::array<std::array<uint8_t,4>,4>& colors) {
        const auto pixels=presentation.readback_color();
        require(pixels.size()==16*16*4,"reference transition readback has the requested extent");
        for (uint32_t y=0; y<16; ++y) for (uint32_t x=0; x<16; ++x)
            for (size_t channel=0; channel<4; ++channel)
                if (pixels[(y*16+x)*4+channel]!=colors[x/4][channel])
                    throw std::runtime_error(std::string("stencil reference stage=")+stage+
                        " x="+std::to_string(x)+" y="+std::to_string(y));
    };
    check_stripes("queued-0-1-1-0", {{{0,0,255,255},{0,255,255,255},
                                         {255,0,0,255},{255,255,255,255}}});
    // End the next list at reference 1, then begin another list with the same
    // pipeline/reference. Both draw entrypoints must restore native state after
    // the command-list reset, even though the previous list ended at 1.
    set(0x824E7390,1); paint(4,12,{1,1,0,1});
    check_stripes("end-at-one", {{{0,0,255,255},{0,255,255,255},
                                   {0,255,255,255},{255,255,255,255}}});
    paint(4,8,{0,1,1,1});
    paint(8,12,{1,0,1,1},true);
    check_stripes("new-list-nonindexed-indexed", {{{0,0,255,255},{255,255,0,255},
                                                    {255,0,255,255},{255,255,255,255}}});
    std::cerr << "PREPARED_STENCIL queued masks/faces/live attachments passed\n";
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
    managed_vertex_readback(graphics, directory, header);
    index_cache_readback(graphics, directory, header);
    index_mapping_lifecycle_readback(graphics, directory, header);
    prepared_dec3n_readback(graphics, directory, header);
    depth_stencil_preservation_readback(graphics, directory, header);
    resolved_allocation_reuse_readback(graphics, directory, header);
    batched_texture_readback(graphics, directory, header);
    prepared_texture_bindings_readback(graphics, directory, header);
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
    short_constant_upload_readback(graphics, directory, header);
}
}
int main() {
    try { run(); std::cout << "Internal resolution readback checks passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
