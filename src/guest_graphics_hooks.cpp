#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_graphics.h"
#include "native_graphics.h"
#include "native_presentation.h"
#include "native_raster_state.h"
#include "native_shaders.h"
#include "native_render_state.h"
#include "render_state_entries.h"
#include "native_renderer.h"
#include "native_formats.h"
#include "guest_execution.h"
#include <plume_render_interface.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <span>
#include <utility>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Generated original bodies; the public sub_* names are overridden below.
PPC_FUNC_IMPL(__imp__sub_824ED770);
PPC_FUNC_IMPL(__imp__sub_824ED588);
PPC_FUNC_IMPL(__imp__sub_82500B38);

namespace sfr {
bool graphics_trace() {
    static const bool on=[]{ const char* t=std::getenv("SFR_TRACE_GRAPHICS"); return !t || *t!='0'; }();
    return on;
}
}

namespace {
sfr::GuestGraphics& graphics() {
    if (!sfr::active_guest_graphics)
        throw sfr::RuntimeStop("native-graphics",0,"native guest graphics owner is unavailable");
    return *sfr::active_guest_graphics;
}

// Writes what the window shows as a BMP (BGRA rows, top first): the
// framebuffer, with the presented area stretched over it as present() does.
void write_framebuffer(const char* path) {
    auto& presentation=graphics().presentation();
    const auto pixels=presentation.readback_color();
    const uint32_t width=presentation.width(), height=presentation.height(), size=width*height*4;
    const auto area=presentation.presented_area();
    std::vector<uint8_t> stretched;
    const uint8_t* rows=pixels.data();
    if(pixels.size()>=size && area.first && area.second && (area.first!=width || area.second!=height)) {
        stretched.resize(size);
        for(uint32_t y=0;y<height;++y) {
            const uint64_t source=uint64_t(y)*area.second/height*width;
            for(uint32_t x=0;x<width;++x)
                std::memcpy(stretched.data()+(uint64_t(y)*width+x)*4,
                            pixels.data()+(source+uint64_t(x)*area.first/width)*4,4);
        }
        rows=stretched.data();
    }
    std::ofstream out(path,std::ios::binary);
    const auto u16=[&](uint16_t v){ out.put(char(v)); out.put(char(v>>8)); };
    const auto u32=[&](uint32_t v){ u16(uint16_t(v)); u16(uint16_t(v>>16)); };
    out.put('B'); out.put('M'); u32(54+size); u32(0); u32(54);
    u32(40); u32(width); u32(uint32_t(-int32_t(height))); u16(1); u16(32); u32(0); u32(size); u32(2835); u32(2835); u32(0); u32(0);
    out.write(reinterpret_cast<const char*>(rows),std::streamsize(size));
}

// SFR_SCREENSHOT=<file.bmp>: write every 60th presented frame (a GPU readback
// each frame would dominate the frame time); the last one written remains.
void save_screenshot() {
    const char* path=std::getenv("SFR_SCREENSHOT");
    if(!path) return;
    static uint32_t presents=0;
    // SFR_SCREENSHOT_EVERY=N changes the interval (default 60 presents).
    static const uint32_t every=[]{ const char* t=std::getenv("SFR_SCREENSHOT_EVERY"); const long n=t?std::strtol(t,nullptr,10):60; return uint32_t(n>0?n:60); }();
    static const uint32_t skip=[]{ const char* t=std::getenv("SFR_SCREENSHOT_SKIP"); return t?uint32_t(std::strtoul(t,nullptr,10)):0u; }();
    if(presents++<skip || presents%every) return;
    // A "%d" in the path numbers the files by present index.
    char numbered[1024];
    if(std::string_view(path).find("%d")!=std::string_view::npos) {
        std::snprintf(numbered,sizeof numbered,path,int(presents));
        path=numbered;
    }
    write_framebuffer(path);
}

void create_shader(PPCContext& ctx,uint8_t* base,sfr::ShaderStage stage,PPCFunc* original) {
    const uint32_t callback_address=stage==sfr::ShaderStage::vertex?0x82B50A30:0x82B50A2C;
    if(!sfr::active_memory) throw sfr::RuntimeStop("native-shader",0,"guest memory is unavailable");
    // Original optional callbacks precede even container validation. Stop here
    // until a nonzero callback is observed and its forwarding ABI is supported.
    const auto callback=sfr::active_memory->load<uint32_t>(callback_address);
    if(callback) throw sfr::RuntimeStop("native-shader-callback",callback,"optional original shader callback is unsupported");
    const uint32_t container=ctx.r3.u32;
    const auto handle=graphics().create_shader(stage,container);
    const auto& shader=graphics().shaders().get(handle);
    // The original body builds the real guest shader object (header copy at
    // +872, microcode in physical memory) that SetVertexShader/SetPixelShader
    // and the effect library read; the native shader is found through it.
    original(ctx,base);
    const uint32_t object=ctx.r3.u32;
    if(!object) throw sfr::RuntimeStop("native-shader",container,"original shader object creation failed");
    graphics().attach_shader(handle,object);
    std::cerr << "NATIVE_SHADER_CREATE stage=" << (stage==sfr::ShaderStage::vertex?"vertex":"pixel")
              << " handle=0x" << std::hex << handle << " object=0x" << object << std::dec
              << " source_bytes=" << shader.entry->source.size()
              << " dxil_bytes=" << shader.entry->dxil.size()
              << " specialization_mask=" << shader.entry->specialization_mask
              << " state=" << (shader.shader?"native-stage":"retained-library") << '\n';
}
void set_render_state(PPCContext& ctx, const char* name, uint32_t address, sfr::RenderState state) {
    sfr::enter_function(ctx,name,address);
    if(!sfr::is_native_render_state_entry(address))
        throw sfr::RuntimeStop("native-graphics-context",address,"unaudited render-state entry");
    if(state == sfr::RenderState::alpha_reference) ctx.fpscr.disableFlushMode();
    graphics().set_render_state(ctx.r3.u32,state,ctx.r4.u32);
    if(sfr::graphics_trace()) std::cerr << "NATIVE_RENDER_STATE source=0x" << std::hex << address
              << " offset=0x" << static_cast<uint32_t>(state) << " value=0x" << ctx.r4.u32
              << std::dec << " retained=1 lr=0x" << std::hex << ctx.lr << std::dec << '\n';
}
void set_blend_request(PPCContext& ctx, const char* name, uint32_t address, sfr::BlendRequest request) {
    sfr::enter_function(ctx,name,address);
    if (!sfr::is_native_render_state_entry(address) || !sfr::active_memory)
        throw sfr::RuntimeStop("native-graphics-context",address,"unaudited blend request entry or unavailable memory");
    const auto update = graphics().set_blend_request(ctx.r3.u32,request,ctx.r4.u32);
    if (!sfr::graphics_trace()) return;
    std::cerr << "NATIVE_BLEND_REQUEST source=0x" << std::hex << address
              << " value=0x" << ctx.r4.u32
              << " requested=0x" << update.requested << " flags=0x" << update.flags;
    if (update.effective_updated) std::cerr << " effective=0x" << update.effective;
    else std::cerr << " effective=unchanged";
    std::cerr << " lr=0x" << ctx.lr << std::dec << " state=retained-for-pipeline\n";
}
void set_sampler_filter(PPCContext& ctx, const char* name, uint32_t address, sfr::SamplerFilter filter) {
    sfr::enter_function(ctx,name,address);
    if (!sfr::is_native_render_state_entry(address))
        throw sfr::RuntimeStop("native-graphics-context",address,"unaudited sampler filter entry");
    const auto state = graphics().set_sampler_filter(ctx.r3.u32,ctx.r4.u32,filter,ctx.r5.u32);
    if(sfr::graphics_trace()) std::cerr << "NATIVE_SAMPLER_FILTER source=0x" << std::hex << address << std::dec
              << " slot=" << ctx.r4.u32 << " value=0x" << std::hex << ctx.r5.u32
              << " word3=0x" << state.word3 << " word4=0x" << state.word4
              << " lr=0x" << ctx.lr << std::dec << " state=retained-fetch-fields\n";
}
}

// Strong public definitions replace only the verified weak generated aliases.
// The original game caller, entry point and all other generated code remain.
SFR_HOOK(sub_824F4CF0) {
    sfr::enter_function(ctx,"sub_824F4CF0",0x824F4CF0);
    ctx.r3.u64=graphics().create_device(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32);
    // The original CreateDevice finishes with 82500B38: every render and
    // sampler state setter with its table default (native setters where
    // hooked), null textures, and SetShaderGPRAllocation(0,0,0). Run it so the
    // device shadow holds the D3D defaults the game relies on.
    const uint64_t lr=ctx.lr;
    ctx.r3.u64=sfr::GuestGraphics::device_address;
    __imp__sub_82500B38(ctx,base);
    ctx.lr=lr;
    ctx.r3.u64=0;
    std::cerr << "NATIVE_CREATE_DEVICE device=0x" << std::hex << sfr::GuestGraphics::device_address
              << std::dec << " width=" << graphics().presentation().width()
              << " height=" << graphics().presentation().height() << " result=0\n";
}

// Device::Reset(device, presentation parameters): the original tears the
// device down (824F8BC0) and creates it again (82500DA0). The native device
// stays; a reset to the parameters it was created with (stored at +0x35BC)
// only restores the default state, as creation does with 82500B38.
SFR_HOOK(sub_824F4858) {
    sfr::enter_function(ctx,"sub_824F4858",0x824F4858);
    auto& memory=*sfr::active_memory;
    const uint32_t device=ctx.r3.u32, parameters=ctx.r4.u32;
    if(device!=sfr::GuestGraphics::device_address || !graphics().created())
        throw sfr::RuntimeStop("native-graphics-reset",device,"reset of a device that is not the native device");
    memory.check(parameters,31*4);
    // A race resets to its own back buffer size (words 0 and 1), multi-sample
    // type (word 4) and presentation interval (word 13). The native device
    // keeps its own swap chain, sample count and presentation; only the
    // parameters the title reads back change.
    for(uint32_t i=0;i<31;++i) {
        const uint32_t wanted=memory.load<uint32_t>(uint64_t(parameters)+i*4);
        const uint32_t current=memory.load<uint32_t>(uint64_t(device)+0x35BC+i*4);
        if(wanted==current) continue;
        if(i!=0 && i!=1 && i!=4 && i!=13) {
            std::cerr << "NATIVE_DEVICE_RESET_UNSUPPORTED word=" << i << " wanted=0x" << std::hex << wanted
                      << " current=0x" << current << std::dec << '\n';
            throw sfr::RuntimeStop("native-graphics-reset",parameters+i*4,"reset changes an unsupported parameter");
        }
        if((i==0 || i==1) && (!wanted || wanted>8192))
            throw sfr::RuntimeStop("native-graphics-reset",parameters+i*4,"reset to unsupported dimensions");
        memory.store<uint32_t>(uint64_t(device)+0x35BC+i*4,wanted);
        std::cerr << "NATIVE_DEVICE_RESET_PARAMETER word=" << i << " value=0x" << std::hex << wanted
                  << " previous=0x" << current << std::dec << '\n';
    }
    const uint64_t lr=ctx.lr;
    ctx.r3.u64=device;
    __imp__sub_82500B38(ctx,base);
    ctx.lr=lr;
    ctx.r3.u64=0;
    std::cerr << "NATIVE_DEVICE_RESET parameters=0x" << std::hex << parameters << std::dec << " result=0\n";
}

// The original makes room by submitting the current command segment to the
// GPU ring. Natively nothing consumes Xenos packets: discard them and return
// the reset write pointer, as the original returns device+48.
SFR_HOOK(sub_824F8720) {
    sfr::enter_function(ctx,"sub_824F8720",0x824F8720);
    uint32_t discarded=0;
    ctx.r3.u64=graphics().discard_commands(ctx.r3.u32,discarded);
    std::cerr << "NATIVE_COMMAND_DISCARD source=0x824f8720 bytes=" << discarded
              << " lr=0x" << std::hex << ctx.lr << std::dec << '\n';
}

// Draws since the last present, reported with each present.
static uint32_t frame_draws=0, frame_textured_draws=0, foreign_draws=0;
// How much vertex data one frame gathers, and its largest single draw: the
// gather is the renderer's main cost, so the present line carries both.
static uint64_t frame_vertex_bytes=0;
static uint32_t frame_cached_draws=0;  // drawn from vertex_cache buffers
static uint32_t frame_biggest_draw=0;
// The distinct stream 0 buffers a frame draws from, and their total size: what
// uploading each stream once a frame would cost instead of gathering every
// draw's vertices (docs/performance.md).
static std::set<std::pair<uint32_t,uint32_t>> frame_streams;
static uint64_t frame_stream_bytes=0;
// Where the indexed draw's time goes, timed rather than sampled: reading the
// index buffer, turning a cut strip into triangles, and gathering vertices.
static uint64_t frame_indices=0;
static double frame_index_ms=0, frame_cut_ms=0, frame_gather_ms=0;
// Cycles this thread actually ran for, beside the wall clock above: a loop
// that takes far longer than it executes is waiting for a core, not working.
static uint64_t frame_index_cycles=0, frame_gather_cycles=0;
// The rest of a draw: its two constant blocks, and recording it.
static double frame_constants_ms=0, frame_record_ms=0, frame_draw_ms=0;
static uint64_t thread_cycles() {
#ifdef _WIN32
    uint64_t cycles=0;
    if(QueryThreadCycleTime(GetCurrentThread(),&cycles)) return cycles;
#endif
    return 0;
}

// SFR_SKIP_DRAWS=1 drops every draw before any work, including gathering its
// vertices: what a frame costs with no renderer at all.
// Scratch that keeps its capacity between draws. Allocating a vector for
// every draw's vertices and indices puts the frame in the C runtime's heap
// lock, which all twenty-odd guest threads share.
static std::span<uint8_t> byte_scratch(size_t bytes,int which=0) {
    static thread_local std::vector<uint8_t> buffers[4];
    auto& buffer=buffers[which];
    if(buffer.size()<bytes) buffer.resize(bytes);
    return {buffer.data(),bytes};
}

// The guests that held the execution permit longest since the last frame,
// as id:milliseconds.
static std::string permit_holders() {
    std::vector<std::pair<uint64_t, int>> held;
    for (int id = 0; id < 64; ++id)
        if (const uint64_t ns = sfr::GuestExecution::held_ns[id].exchange(0, std::memory_order_relaxed))
            held.push_back({ns, id});
    std::sort(held.rbegin(), held.rend());
    std::string text;
    char item[32];
    for (size_t i = 0; i < held.size() && i < 5; ++i) {
        std::snprintf(item, sizeof item, "%s%d:%.2f", i ? "," : "", held[i].second, double(held[i].first) / 1e6);
        text += item;
    }
    return text;
}

static bool skip_draws() {
    static const bool skip=[]{ const char* t=std::getenv("SFR_SKIP_DRAWS"); return t && *t!='0'; }();
    return skip;
}

// SFR_RENDER_EVERY=N (testing): during a race, draw and present only every
// Nth frame. A race steps its game a sixtieth of a second per frame whatever
// the frame rate, so at 22 fps it runs at a third of its speed; skipping the
// rendering of the frames in between lets it advance faster while the screen
// updates less often. Clears, draws, resolves and the present are skipped on
// those frames -- none of them writes guest memory -- and the frame's GPU
// callbacks still run. Menus render every frame: they already run at 60,
// and parts of them keep real time, which frames run twice as fast would
// break (the title ignored its START). 1, the default, renders every frame.
static bool rendering_this_frame() {
    static const uint32_t every=[]{
        const char* t=std::getenv("SFR_RENDER_EVERY");
        const long n=t?std::strtol(t,nullptr,10):1;
        return uint32_t(n>1?n:1);
    }();
    if(every==1) return true;
    constexpr uint32_t race_flag=0x83E52F8C;  // nonzero during a race (nui_race_hooks.cpp)
    if(!sfr::active_memory->load<uint32_t>(race_flag)) return true;
    return sfr::present_count%every==0;
}

// SFR_FRAME_LIMIT=<fps>: at most that many presents a second. The title
// steps a race a sixtieth of a second per frame and does not wait for the
// display there, so a race that runs faster than 60 fps (drawing every
// second frame, SFR_RENDER_EVERY) runs faster than real time. The wait
// releases the execution permit. Off by default: tests want speed.
static void limit_frame_rate() {
    static const double fps=[]{ const char* t=std::getenv("SFR_FRAME_LIMIT"); return t?std::strtod(t,nullptr):0.0; }();
    if(fps<=0) return;
    using clock=std::chrono::steady_clock;
    static const auto period=std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0/fps));
    static clock::time_point next{};
    const auto now=clock::now();
    if(next==clock::time_point{} || now>next+4*period) next=now;  // start, or far behind: no catching up
    if(now<next) {
        struct Until { clock::time_point at; } until{next};
        sfr::wait_without_permit([](void* argument) {
            const auto at=static_cast<Until*>(argument)->at;
            // Sleep to within about a millisecond (the sleep granularity), then yield.
            while(clock::now()+std::chrono::milliseconds(2)<at) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            while(clock::now()<at) std::this_thread::yield();
        },&until);
    }
    next+=period;
}

// The original saves viewport/scissor, binds the back buffer as RT0, resolves
// it into the front buffer, restores the saved state and swaps the front buffer
// (VdSwap/VdPersistDisplay). Viewport, scissor and RT0 end unchanged.
namespace sfr { std::atomic<uint32_t> present_count{0}; }
// Time base for the present log (seconds since the runtime started).
static const auto process_start=std::chrono::steady_clock::now();
// GPU completion callbacks (InsertCallback) waiting for the frame's end.
static std::vector<std::pair<uint32_t,uint32_t>> frame_callbacks;

SFR_HOOK(sub_824E65A0) {
    sfr::enter_function(ctx,"sub_824E65A0",0x824E65A0);
    save_screenshot();
    // Submitting the frame and waiting for it: what the CPU spends beyond
    // recording, which is where a GPU-bound frame shows up.
    const auto present_start=std::chrono::steady_clock::now();
    if(rendering_this_frame()) graphics().present_front_buffer(ctx.r3.u32);
    const double present_ms=
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-present_start).count();
    // The window belongs to this thread (CreateDevice ran here): handle its
    // messages every frame so Windows does not mark it unresponsive, and end
    // the run when the player closes it.
    graphics().presentation().pump_events();
    limit_frame_rate();
    if(graphics().presentation().close_requested())
        throw sfr::RuntimeStop("window-closed",0,"the game window was closed");
    // The frame's commands have completed: run its callbacks here, as the
    // GPU interrupt would before the next frame. (The movie player keeps at
    // most three frames in flight and otherwise waits for these callbacks.)
    const auto callbacks=std::exchange(frame_callbacks,{});
    for(const auto& [routine,argument]:callbacks) {
        PPCContext saved=ctx;
        ctx.r3.u64=argument;
        sfr::call_indirect(ctx,base,routine);
        ctx=saved;
    }
    // One line a frame: the per-frame timings a profile run reads.
    const auto pipeline_work=graphics().renderer().take_pipeline_work();
    std::cerr << "NATIVE_PRESENT source=0x824e65a0 device=0x" << std::hex << ctx.r3.u32
              << " lr=0x" << ctx.lr << std::dec << " draws=" << frame_draws
              << " textured=" << frame_textured_draws << " foreign=" << foreign_draws
              << " vertex_bytes=" << frame_vertex_bytes << " cached_draws=" << frame_cached_draws
              << " biggest=" << frame_biggest_draw
              << " streams=" << frame_streams.size() << " stream_bytes=" << frame_stream_bytes
              << " indices=" << frame_indices << " index_ms=" << frame_index_ms << " cut_ms=" << frame_cut_ms
              << " gather_ms=" << frame_gather_ms << " constants_ms=" << frame_constants_ms
              << " record_ms=" << frame_record_ms << " draw_ms=" << frame_draw_ms
              << " pipelines=" << pipeline_work.created << " pipeline_ms=" << pipeline_work.milliseconds
              << " ring_flushes=" << pipeline_work.ring_flushes
              << " textures=" << pipeline_work.textures << " texture_ms=" << pipeline_work.texture_milliseconds
              << " present_ms=" << present_ms << " main_queued_ms="
              << double(sfr::GuestExecution::main_thread_ready_wait_ns.exchange(0,std::memory_order_relaxed))/1e6
              << " main_blocked_ms="
              << double(sfr::GuestExecution::main_thread_blocked_ns.exchange(0,std::memory_order_relaxed))/1e6
              << " gpu_wait_ms=" << double(sfr::main_gpu_wait_ns.exchange(0,std::memory_order_relaxed))/1e6
              << " holders=" << permit_holders()
              << " presented=1 seconds="
              << std::chrono::duration<double>(std::chrono::steady_clock::now()-process_start).count() << '\n';
    frame_draws=frame_textured_draws=foreign_draws=0;
    frame_vertex_bytes=0;
    frame_cached_draws=0;
    sfr::active_memory->advance_write_epoch();  // vertex_cache's frames
    frame_biggest_draw=0;
    frame_streams.clear();
    frame_stream_bytes=0;
    frame_indices=0;
    frame_index_ms=frame_cut_ms=frame_gather_ms=0;
    frame_index_cycles=frame_gather_cycles=0;
    frame_constants_ms=frame_record_ms=frame_draw_ms=0;
    ++sfr::present_count;
    // SFR_MEMORY_DUMP=P1,P2,... (investigation): at those presents, every
    // readable guest page from 0x40000000 up (heaps, stacks, the image's data,
    // physical allocations) goes to out/memdump-P.bin as (big-endian u32
    // address, 4096 bytes) records, for finding game state by comparing dumps.
    static const std::vector<uint32_t> dumps=[]{
        std::vector<uint32_t> at;
        if(const char* t=std::getenv("SFR_MEMORY_DUMP"))
            for(const char* p=t; *p; ) { char* end; at.push_back(uint32_t(std::strtoul(p,&end,10))); p=*end?end+1:end; }
        return at;
    }();
    if(!dumps.empty() && std::find(dumps.begin(),dumps.end(),sfr::present_count.load())!=dumps.end()) {
        const auto& memory=*sfr::active_memory;
        std::ofstream out("out/memdump-"+std::to_string(sfr::present_count.load())+".bin",std::ios::binary);
        for(uint64_t page=0x40000000; page<0x100000000ull; page+=4096) {
            if(!memory.readable(page,4096)) continue;
            const uint8_t address[4]={uint8_t(page>>24),uint8_t(page>>16),uint8_t(page>>8),uint8_t(page)};
            out.write(reinterpret_cast<const char*>(address),4);
            out.write(reinterpret_cast<const char*>(memory.base()+page),4096);
        }
        std::cerr<<"MEMORY_DUMP present="<<sfr::present_count.load()<<'\n';
    }
    // SFR_POKE=addr=value@present,... (testing): stores the big-endian word at
    // the hex guest address when that present is reached, e.g. to fill a
    // mission's ring counter so a scripted Grand Prix can go on.
    struct Poke { uint32_t address, value, present; };
    static const std::vector<Poke> pokes=[]{
        std::vector<Poke> list;
        if(const char* t=std::getenv("SFR_POKE"))
            for(const char* p=t; *p; ) {
                char* end;
                Poke poke{};
                poke.address=uint32_t(std::strtoul(p,&end,16)); if(*end!='=') break;
                poke.value=uint32_t(std::strtoul(end+1,&end,10)); if(*end!='@') break;
                poke.present=uint32_t(std::strtoul(end+1,&end,10));
                list.push_back(poke);
                p=*end?end+1:end;
            }
        return list;
    }();
    for(const auto& poke:pokes)
        if(poke.present==sfr::present_count.load() && sfr::active_memory->readable(poke.address,4)) {
            sfr::active_memory->store<uint32_t>(poke.address,poke.value);
            std::cerr<<"POKE address=0x"<<std::hex<<poke.address<<std::dec<<" value="<<poke.value<<'\n';
        }
    // SFR_PRESENT_LIMIT=N ends the run after N presents (debugging aid).
    static const uint32_t limit=[]{ const char* t=std::getenv("SFR_PRESENT_LIMIT"); return t?uint32_t(std::strtoul(t,nullptr,10)):0u; }();
    if(limit && sfr::present_count>=limit)
        throw sfr::RuntimeStop("present-limit",limit,"SFR_PRESENT_LIMIT reached");
}

// DrawVerticesUP(device, primitive, vertex count, data, stride). The original
// flushes dirty state as Xenos packets, copies the vertices inline into the
// command buffer and appends a draw. The device shadow mirrors those GPU
// registers, so the native draw reads its state from the same fields.
// Shared native draw: count vertices of stride bytes (guest byte order).
static void native_draw(PPCContext& ctx, uint32_t source, uint32_t device, uint32_t primitive,
                        uint32_t count, std::span<const uint8_t> vertices, uint32_t stride,
                        std::span<const uint32_t> indices={}, int32_t base_vertex_location=0,
                        uint32_t source_physical=0) {
    auto& memory=*sfr::active_memory;
    if(skip_draws() || !rendering_this_frame()) return;
    // Everything a draw costs beside reading its indices and vertices.
    const auto draw_start=std::chrono::steady_clock::now();
    struct DrawTimer {
        std::chrono::steady_clock::time_point start;
        ~DrawTimer() {
            frame_draw_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        }
    } draw_timer{draw_start};
    if(device!=sfr::GuestGraphics::device_address) throw sfr::RuntimeStop("native-draw",device,"draw on a non-native device");
    // Reused: its two constant arrays are four kilobytes each, and zeroing
    // them and rebuilding the element list for every draw is a frame's worth
    // of work at eight hundred draws. Everything below assigns what it needs;
    // these three are the only fields a draw may leave alone.
    static thread_local sfr::NativeDraw draw;
    draw.elements.clear();
    draw.palette={};
    draw.shared={};
    draw.cull=plume::RenderCullMode::NONE;
    switch(primitive) {
    case 1: draw.topology=plume::RenderPrimitiveTopology::POINT_LIST; break;
    case 2: draw.topology=plume::RenderPrimitiveTopology::LINE_LIST; break;
    case 3: draw.topology=plume::RenderPrimitiveTopology::LINE_STRIP; break;
    case 4: draw.topology=plume::RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case 5: draw.topology=plume::RenderPrimitiveTopology::TRIANGLE_FAN; break;
    case 6: draw.topology=plume::RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    default: throw sfr::RuntimeStop("native-draw",primitive,"unsupported primitive type");
    }
    // Draws aimed at one of the title's own surfaces share the one native
    // framebuffer: each pass draws there and the resolve that follows copies
    // the result into the texture the next pass samples, so the chain works
    // without real offscreen targets. SFR_SKIP_FOREIGN_TARGETS=1 drops them
    // instead, which shows the scene pass on its own.
    if(sfr::GuestGraphics::foreign_render_targets &&
       memory.load<uint32_t>(uint64_t(device)+0x3148)!=sfr::GuestGraphics::color_handle) {
        static const bool skip=[]{ const char* t=std::getenv("SFR_SKIP_FOREIGN_TARGETS"); return t && *t!='0'; }();
        if(skip) { ++foreign_draws; return; }
    }
    // A sanity cap on one gathered blob, not a hardware limit: the race draws
    // 56913 vertices of 20 bytes in one go, which is already past a megabyte.
    if(!count || !stride || stride>256 || uint64_t(count)*stride>0x800000)
        throw sfr::RuntimeStop("native-draw",count,"unsupported vertex count or stride");
    if(vertices.size()!=size_t(count)*stride) throw sfr::RuntimeStop("native-draw",count,"vertex data size mismatch");
    frame_vertex_bytes+=vertices.size();
    frame_biggest_draw=(std::max)(frame_biggest_draw,count);
    draw.vertex_count=count;
    draw.stride=stride;
    draw.indices=indices;
    draw.base_vertex_location=base_vertex_location;

    // Declaration elements (+11992: object with count at +24, 12-byte elements at +52).
    const uint32_t declaration=memory.load<uint32_t>(uint64_t(device)+11992);
    if(!declaration) throw sfr::RuntimeStop("native-draw",device,"draw without a vertex declaration");
    const uint32_t elements=memory.load<uint32_t>(uint64_t(declaration)+24);
    if(elements>16) throw sfr::RuntimeStop("native-draw",elements,"vertex declaration is too large");
    // Locations must match XenosRecomp (as in Marathon Recompiled).
    struct Location { uint32_t usage, index; };
    constexpr Location locations[]={{0,0},{0,1},{0,2},{0,3},{3,0},{3,1},{3,2},{3,3},{6,0},{6,1},{6,2},{6,3},
                                    {7,0},{5,0},{5,1},{5,2},{5,3},{10,0},{2,0},{1,0}};
    uint32_t* swapped[8]={};
    swapped[5]=&draw.shared.swapped_texcoords; swapped[3]=&draw.shared.swapped_normals;
    swapped[7]=&draw.shared.swapped_binormals; swapped[6]=&draw.shared.swapped_tangents;
    swapped[1]=&draw.shared.swapped_blend_weights;
    // DEC3N (signed normalized 10:10:10) has no D3D12 format: each such
    // element is converted to four SNORM16 components appended to its vertex.
    std::vector<uint32_t> dec3n_offsets;
    for(uint32_t i=0;i<elements;++i) {
        const uint64_t e=uint64_t(declaration)+52+12*i;
        const uint32_t stream=memory.load<uint16_t>(e), offset=memory.load<uint16_t>(e+2);
        const uint32_t type=memory.load<uint32_t>(e+4), usage=memory.load<uint8_t>(e+9), index=memory.load<uint8_t>(e+10);
        constexpr uint32_t dec3n=0x2A2187;
        const auto format=type==dec3n
            ? std::optional<sfr::DeclarationFormat>(sfr::DeclarationFormat{plume::RenderFormat::R16G16B16A16_SNORM,false})
            : sfr::declaration_format(type);
        const auto input=sfr::shader_input_usage(usage,index);
        const char* semantic=sfr::declaration_semantic(input.usage);
        if(stream!=0 || !format || !semantic)
            throw sfr::RuntimeStop("native-draw",type,"unsupported vertex element for an up draw");
        uint32_t location=~0u;
        for(uint32_t l=0;l<std::size(locations);++l)
            if(locations[l].usage==input.usage && locations[l].index==input.index) location=l;
        if(location==~0u) continue;  // bound but read by no XenosRecomp shader
        uint32_t element_offset=offset;
        if(type==dec3n) {
            if(offset+4>stride) throw sfr::RuntimeStop("native-draw",offset,"vertex element exceeds the stride");
            element_offset=stride+8*uint32_t(dec3n_offsets.size());
            dec3n_offsets.push_back(offset);
        }
        // Vulkan wants an attribute's offset aligned to its component size.
        // Desktop drivers fetch an under-aligned one anyway; an Adreno need
        // not, which would draw some meshes as scattered triangles. Reported
        // once per (type, offset) so a device's log says whether that is it.
        if(const uint32_t component=sfr::format_component_bytes(format->format);
           component>1 && element_offset%component) {
            static std::set<std::pair<uint32_t,uint32_t>> misaligned;
            static std::mutex misaligned_lock;
            std::lock_guard guard(misaligned_lock);
            if(misaligned.size()<32 && misaligned.insert({type,element_offset}).second)
                std::cerr << "NATIVE_VERTEX_MISALIGNED type=0x" << std::hex << type << " offset=" << std::dec
                          << element_offset << " component=" << component << " stride=" << stride
                          << " semantic=" << semantic << input.index << '\n';
        }
        draw.elements.emplace_back(semantic,input.index,location,format->format,0,element_offset);
        if(format->swapped_pairs && input.usage<8 && swapped[input.usage]) *swapped[input.usage]|=1u<<input.index;
    }
    // SFR_VERTEX_CACHE=1: vertices read in place from a GPU buffer (the
    // physical address is known) are kept from frame to frame while nothing
    // stores to them (NativeRenderer::vertex_cache).
    static const bool vertex_cache=[]{ const char* t=std::getenv("SFR_VERTEX_CACHE"); return t && *t!='0'; }();
    const uint32_t wide=stride+8*uint32_t(dec3n_offsets.size());
    sfr::NativeRenderer::CachedVertices cached;
    if(vertex_cache && source_physical) {
        uint64_t layout=stride;
        for(const uint32_t offset:dec3n_offsets) layout=layout*1000003u+offset+1;
        cached=graphics().renderer().vertex_cache(memory,source_physical,vertices.size(),uint64_t(count)*wide,layout);
    }
    draw.vertex_buffer=cached.buffer;
    if(cached.buffer && cached.fill.empty()) {
        // Already in its buffer, in host form.
        draw.vertices={};
        draw.stride=wide;
        ++frame_cached_draws;
    } else {
    // The vertices in host byte order. Unless an element needs repacking they
    // are swapped straight into the renderer's upload ring (or the cached
    // buffer being filled): copying them into scratch, swapping them there and
    // copying them into the ring passed over a race frame's twenty-odd
    // megabytes three times.
    {
        std::span<uint8_t> host;
        if(dec3n_offsets.empty())
            host=cached.fill.empty()?graphics().renderer().vertex_space(vertices.size(),indices.size()*4):cached.fill;
        if(host.size()!=vertices.size()) host=byte_scratch(vertices.size(),3);
        sfr::swap_words_into(host,vertices);
        draw.vertices=host;
    }
    if(!dec3n_offsets.empty()) {
        // Repacked straight into the upload ring (or the cached buffer) when
        // it has room (write only: both are write-combined, and the loop
        // below only writes them).
        std::span<uint8_t> repacked=cached.fill.empty()
            ? graphics().renderer().vertex_space(uint64_t(count)*wide,indices.size()*4) : cached.fill;
        if(repacked.size()!=size_t(count)*wide) repacked=byte_scratch(size_t(count)*wide,1);
        // Each component is one of 1024 values, so its SNORM16 form is looked
        // up: rounding it per vertex was a tenth of a race frame's main
        // thread (docs/performance.md).
        static const std::array<int16_t,1024> snorm16=[] {
            std::array<int16_t,1024> table{};
            for(uint32_t bits=0;bits<1024;++bits) {
                const int32_t value=int32_t(bits<<22)>>22;  // sign-extended 10 bits
                const float unit=(std::max)(float(value)/511.0f,-1.0f);
                table[bits]=int16_t(std::lround(unit*32767.0f));
            }
            return table;
        }();
        for(uint32_t v=0;v<count;++v) {
            const uint8_t* from=draw.vertices.data()+size_t(v)*stride;
            uint8_t* to=repacked.data()+size_t(v)*wide;
            std::memcpy(to,from,stride);
            for(size_t j=0;j<dec3n_offsets.size();++j) {
                uint32_t word;
                std::memcpy(&word,from+dec3n_offsets[j],4);  // host order after swap_words
                const int16_t out[4]={snorm16[word&1023],snorm16[(word>>10)&1023],snorm16[(word>>20)&1023],32767};
                std::memcpy(to+stride+8*j,out,8);
            }
        }
        draw.vertices=repacked;
        draw.stride=wide;
    }
    }
    for(uint32_t l=0;l<std::size(locations);++l) {
        if(std::any_of(draw.elements.begin(),draw.elements.end(),[&](const auto& e){ return e.location==l; })) continue;
        const uint32_t usage=locations[l].usage;
        const bool vector3=usage==3 || usage==6 || usage==7 || usage==2;
        draw.elements.emplace_back(sfr::declaration_semantic(usage),locations[l].index,l,
            vector3?plume::RenderFormat::R32G32B32_FLOAT:plume::RenderFormat::R32_FLOAT,15,0);
    }

    // Shaders: objects at +12872 (VS) and +12868 (PS) map to native shaders.
    const auto& shaders=graphics().shaders();
    const uint32_t vs_object=memory.load<uint32_t>(uint64_t(device)+12872), ps_object=memory.load<uint32_t>(uint64_t(device)+12868);
    if(!shaders.owns_object(vs_object) || !shaders.owns_object(ps_object))
        throw sfr::RuntimeStop("native-draw",vs_object,"draw shaders are not native-backed");
    const auto& vs=shaders.get(shaders.handle_of(vs_object));
    const auto& ps=shaders.get(shaders.handle_of(ps_object));
    const bool vulkan=sfr::selected_graphics_backend()==sfr::GraphicsBackend::vulkan;
    if(vs.entry->code(vulkan).empty() || ps.entry->code(vulkan).empty()) {
        // The pinned translator rejected one of the shaders (see RUNTIME_SHADER).
        static std::set<std::array<uint32_t,3>> reported;
        if(reported.insert({vs_object,ps_object,uint32_t(ctx.lr)}).second)
            std::cerr << "NATIVE_DRAW_SKIPPED source=0x" << std::hex << source << " vs=0x" << vs_object
                      << " ps=0x" << ps_object << " lr=0x" << ctx.lr << " vs_code=" << !vs.entry->code(vulkan).empty()
                      << " ps_code=" << !ps.entry->code(vulkan).empty() << " stream1=0x"
                      << memory.load<uint32_t>(uint64_t(device)+0x770) << "/0x"
                      << memory.load<uint32_t>(uint64_t(device)+0x774) << std::dec
                      << " stride1=" << uint32_t(memory.load<uint8_t>(uint64_t(device)+12705))*4
                      << " reason=untranslatable-shader\n";
        return;
    }
    if(!vs.shader) throw sfr::RuntimeStop("native-draw",vs_object,"specialized vertex shaders are unsupported");
    const auto& state=graphics().render_state();
    const bool alpha_test=state.alpha_test_enabled.value_or(false);
    draw.vertex_shader=vs.shader.get();
    // Vulkan creates every pixel shader and specializes its pipeline; D3D12
    // links a specialized shader per constant value.
    draw.pixel_shader=ps.shader?ps.shader.get():graphics().renderer().specialized(*ps.entry,alpha_test?2u:0u);
    draw.pixel_spec_constants=vulkan?(alpha_test?2u:0u)&ps.entry->specialization_mask:0u;

    // Skinning palette: a vertex shader that fetches bone matrices reads them
    // from the buffer SetStreamSource bound to stream 1 (vertex fetch constant
    // 94 at +0x770, the same layout as stream 0). Whole entries only, and at
    // least one zero entry so the shader never reads an unbound constant buffer.
    if(vs.entry->vertex_palette_float4s) {
        const uint32_t entry=vs.entry->vertex_palette_float4s*16;
        const uint32_t fetch0=memory.load<uint32_t>(uint64_t(device)+0x770);
        const uint32_t fetch1=memory.load<uint32_t>(uint64_t(device)+0x774);
        uint32_t bytes=(fetch0&3)==3 && (fetch1&3)==2 ? (std::min)(fetch1&0x03FFFFFCu,16384u) : 0;
        bytes-=bytes%entry;
        const std::span<uint8_t> palette=byte_scratch((std::max)(bytes,entry),2);
        if(bytes) {
            const uint32_t data=sfr::NativeRenderer::guest_address(memory,fetch0&0x1FFFFFFCu,bytes);
            memory.check(data,bytes);
            std::memcpy(palette.data(),memory.base()+data,bytes);
        }
        // The entry past the copied rows is the zero one the shader may read.
        std::fill(palette.begin()+bytes,palette.end(),uint8_t{0});
        sfr::swap_words(palette);
        draw.palette=palette;
        // Keyed on whether a stream was bound as well: a shader's first draw
        // may be a pass that binds none while a later one does.
        static std::set<uint64_t> palettes;
        if(palettes.insert(uint64_t(bytes!=0)<<32 | vs_object).second) {
            std::cerr << "NATIVE_PALETTE vs=0x" << std::hex << vs_object << " stream=0x" << (fetch0&0x1FFFFFFCu)
                      << std::dec << " bytes=" << bytes << " entry=" << entry;
            // Without a stream 1, name the vertex fetch constants that do hold
            // a buffer (the fetch constant array is device +0x480, 8 bytes each).
            if(!bytes)
                for(uint32_t slot=48;slot<96;++slot) {
                    const uint32_t word=memory.load<uint32_t>(uint64_t(device)+0x480+slot*8);
                    if(word) std::cerr << " slot" << slot << "=0x" << std::hex << word << std::dec;
                }
            std::cerr << '\n';
        }
    }

    // Constants: VS float4 c0..c255 at +1920, PS at +6016, big-endian words.
    // Copied in one block each: a thousand checked loads per draw was a
    // quarter of the time a draw took.
    const auto constants=[&](uint32_t offset,std::array<uint32_t,1024>& into) {
        memory.check(uint64_t(device)+offset,into.size()*4);
        std::memcpy(into.data(),memory.base()+uint64_t(device)+offset,into.size()*4);
        sfr::swap_words({reinterpret_cast<uint8_t*>(into.data()),into.size()*4});
    };
    const auto constants_start=std::chrono::steady_clock::now();
    constants(1920,draw.vertex_constants);
    constants(6016,draw.pixel_constants);
    frame_constants_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-constants_start).count();
    // Loop constants i0..i15 (+10140, one packed register each: count, start
    // and step as signed bytes) for a shader whose loops count with them.
    for(uint32_t i=0;i<16;++i) {
        const uint32_t packed=memory.load<uint32_t>(uint64_t(device)+10140+i*4);
        for(uint32_t c=0;c<4;++c) draw.loop_constants[i*4+c]=int8_t(packed>>(8*c));
    }
    // Samplers s0..s15 read fetch constants 0..15 as the GPU does, not the
    // binding table: SetTexture(NULL) only clears the fetch type to 0
    // ("invalid texture") and leaves the rest of the constant. Xenos still
    // samples such a constant (Xenia's gpu_allow_invalid_fetch_constants), and
    // the effect framework commits NULL for samplers the game binds directly.
    // The same fetch words get the same texture and sampler until the
    // renderer's texture state moves: a race frame's draws bind mostly the
    // same textures, and looking each slot up again was a millisecond a frame.
    struct SlotMemo { std::array<uint32_t,6> words{}; uint64_t generation=~uint64_t(0); uint32_t texture=0, sampler=0; };
    static thread_local std::array<SlotMemo,16> memo;
    const uint64_t generation=graphics().renderer().texture_generation();
    for(uint32_t slot=0;slot<16;++slot) {
        const auto words=graphics().texture_fetch(slot);
        draw.shared.texture_2d_array[slot]=1;
        draw.shared.texture_cube[slot]=2;
        auto& remembered=memo[slot];
        if(remembered.generation==generation && std::equal(words.begin(),words.end(),remembered.words.begin())) {
            draw.shared.texture_2d[slot]=remembered.texture;
            draw.shared.sampler[slot]=remembered.sampler;
            continue;
        }
        const uint32_t type=words[0]&3;
        const bool sampled=(type==2 || type==0) && (words[1]&~0xFFFu);
        // A slot the draw reads as unbound although a constant is there: the
        // first few are reported so a missing render-target texture shows up.
        if(!sampled && (words[0]||words[1])) {
            static std::set<std::array<uint32_t,2>> unbound;
            if(unbound.size()<32 && unbound.insert({words[0],words[1]}).second)
                std::cerr << "NATIVE_TEXTURE_UNBOUND slot=" << slot << " type=" << type
                          << std::hex << " word0=0x" << words[0] << " word1=0x" << words[1]
                          << " base=0x" << (words[1]&~0xFFFu) << std::dec << '\n';
        }
        draw.shared.texture_2d[slot]=sampled?graphics().renderer().texture(memory,words):0;
        draw.shared.sampler[slot]=sampled?graphics().renderer().sampler(words):0;
        // Taken after the lookups, which may themselves move the state.
        remembered={words,graphics().renderer().texture_generation(),draw.shared.texture_2d[slot],draw.shared.sampler[slot]};
    }
    // Bool constants (register 0x4900 at +10112): VS b0.. in word 0, PS b128.. in word 4.
    draw.shared.booleans=(memory.load<uint32_t>(uint64_t(device)+10112)&0xFFFF) |
                         ((memory.load<uint32_t>(uint64_t(device)+10112+16)&0xFFFF)<<16);
    const auto& presentation=graphics().presentation();
    draw.shared.half_pixel_offset[0]=1.0f/float(presentation.width());
    draw.shared.half_pixel_offset[1]=-1.0f/float(presentation.height());
    draw.shared.alpha_threshold=state.alpha_reference.value_or(0.0f);
    // PA_CL_VTE_CNTL shadow (+10572, set by SetRenderState(VIEWPORTENABLE)):
    // without the viewport scale/offset bits the shader output is in pixels.
    if(!(memory.load<uint32_t>(uint64_t(device)+10572)&0x3F)) {
        draw.shared.screen_space_scale[0]=2.0f/float(presentation.width());
        draw.shared.screen_space_scale[1]=-2.0f/float(presentation.height());
    }

    draw.blend=graphics().blend_control(0);
    // D3DRS_COLORWRITEENABLE shadow (+12036). The original SetRenderTarget copies
    // it into RB_COLOR_MASK (+10460) for a bound RT0; the native device binds RT0
    // without running it, so read the render state directly.
    draw.write_mask=uint8_t(memory.load<uint32_t>(uint64_t(device)+12036)&0xF);
    draw.depth_enabled=state.depth_enable_requested.value_or(false) &&
                       memory.load<uint32_t>(uint64_t(device)+0x3158)==sfr::GuestGraphics::depth_handle;
    draw.depth_write=draw.depth_enabled && state.depth_write_enabled.value_or(false);
    draw.depth_function=state.depth_function.value_or(plume::RenderComparisonFunction::LESS_EQUAL);
    // Stencil: the setters are not bridged, so it is read from the device's
    // register shadows, RB_DEPTHCONTROL (+10548) and RB_STENCILREFMASK
    // (+10496). The title writes a HUD gauge's shape into the stencil and
    // draws the fill where it is set; without the test the gauge was full.
    {
        const uint32_t control=memory.load<uint32_t>(uint64_t(device)+10548);
        const uint32_t refmask=memory.load<uint32_t>(uint64_t(device)+10496);
        const bool target=memory.load<uint32_t>(uint64_t(device)+0x3158)==sfr::GuestGraphics::depth_handle;
        draw.stencil_enabled=target && (control&1);
        if(draw.stencil_enabled) {
            using Compare=plume::RenderComparisonFunction;
            using Op=plume::RenderStencilOp;
            static constexpr Compare compare[8]={Compare::NEVER,Compare::LESS,Compare::EQUAL,Compare::LESS_EQUAL,
                                                 Compare::GREATER,Compare::NOT_EQUAL,Compare::GREATER_EQUAL,Compare::ALWAYS};
            static constexpr Op op[8]={Op::KEEP,Op::ZERO,Op::REPLACE,Op::INCREMENT_AND_CLAMP,Op::DECREMENT_AND_CLAMP,
                                       Op::INVERT,Op::INCREMENT_AND_WRAP,Op::DECREMENT_AND_WRAP};
            const auto face=[&](uint32_t shift) {
                return plume::RenderStencilFaceDesc{op[(control>>(shift+6))&7],op[(control>>(shift+3))&7],
                                                    op[(control>>(shift+9))&7],compare[(control>>shift)&7]};
            };
            draw.stencil_front=face(8);                        // function, fail, z pass, z fail
            draw.stencil_back=(control&0x80)?face(20):draw.stencil_front;  // BACKFACE_ENABLE
            draw.stencil_reference=uint8_t(refmask);
            draw.stencil_read_mask=uint8_t(refmask>>8);
            draw.stencil_write_mask=uint8_t(refmask>>16);
        }
    }
    if(state.cull) {
        auto mode=state.cull->mode;
        // Plume pipelines treat clockwise as front.
        if(state.cull->front_counter_clockwise && mode!=plume::RenderCullMode::NONE)
            mode=mode==plume::RenderCullMode::BACK?plume::RenderCullMode::FRONT:plume::RenderCullMode::BACK;
        draw.cull=mode;
    }
    const auto record_start=std::chrono::steady_clock::now();
    graphics().renderer().draw(draw);
    frame_record_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-record_start).count();
    ++frame_draws;
    // SFR_FRAME_DUMP=<present>: write the framebuffer every SFR_FRAME_DUMP_EVERY
    // draws of that one frame, to see which draw changes what is on screen.
    static const uint32_t dump_present=[]{ const char* t=std::getenv("SFR_FRAME_DUMP"); return t?uint32_t(std::strtoul(t,nullptr,10)):0u; }();
    if(dump_present && sfr::present_count==dump_present) {
        static const uint32_t every=[]{ const char* t=std::getenv("SFR_FRAME_DUMP_EVERY"); const long n=t?std::strtol(t,nullptr,10):10; return uint32_t(n>0?n:10); }();
        if(frame_draws%every==0) {
            char path[256];
            std::snprintf(path,sizeof path,"out/frame-%03u.bmp",frame_draws);
            write_framebuffer(path);
            std::cerr << "NATIVE_FRAME_DUMP draw=" << frame_draws << " path=" << path
                      << " vs=0x" << std::hex << vs_object << " ps=0x" << ps_object
                      << " target=0x" << memory.load<uint32_t>(uint64_t(device)+0x3148) << std::dec
                      << " vertices=" << draw.vertex_count << " topology=" << uint32_t(draw.topology)
                      << " blend=" << draw.blend.blendEnabled << " mask=0x" << std::hex
                      << uint32_t(draw.write_mask) << std::dec << " depth=" << draw.depth_enabled
                      << " textures=" << std::hex;
            for(uint32_t slot=0;slot<4;++slot)
                std::cerr << (slot?",":"") << "0x" << (graphics().texture_fetch(slot)[1]&0xFFFFF000u);
            std::cerr << std::dec << '\n';
        }
    }
    // SFR_DRAW_DUMP=N prints the vertices (as floats) of the first N draws.
    static const long draw_dump=[]{ const char* t=std::getenv("SFR_DRAW_DUMP"); return t?std::strtol(t,nullptr,10):0L; }();
    static long dumped=0;
    // SFR_DRAW_DUMP_SOURCE=<hex> limits the dump to one draw entry point.
    static const uint32_t dump_source=[]{ const char* t=std::getenv("SFR_DRAW_DUMP_SOURCE"); return t?uint32_t(std::strtoul(t,nullptr,16)):0u; }();
    if(dumped<draw_dump && (!dump_source || dump_source==source)) {
        ++dumped;
        std::cerr << "NATIVE_DRAW_DUMP index=" << dumped << " lr=0x" << std::hex << ctx.lr << std::dec << " floats=";
        for(size_t i=0;i+4<=draw.vertices.size();i+=4)
            std::cerr << (i?",":"") << std::bit_cast<float>(uint32_t(draw.vertices[i]|draw.vertices[i+1]<<8|draw.vertices[i+2]<<16|draw.vertices[i+3]<<24));
        std::cerr << " vs_c0..c7=";
        for(uint32_t i=0;i<32;++i) std::cerr << (i?",":"") << std::bit_cast<float>(draw.vertex_constants[i]);
        std::cerr << " ps_c0..c3=";
        for(uint32_t i=0;i<16;++i) std::cerr << (i?",":"") << std::bit_cast<float>(draw.pixel_constants[i]);
        std::cerr << " elements=";
        for(const auto& e:draw.elements) if(e.slotIndex==0) std::cerr << e.semanticName << e.semanticIndex << "@" << e.location << "/s" << e.slotIndex << "+" << e.alignedByteOffset << ";";
        std::cerr << '\n';
    }
    static const bool frame_trace=std::getenv("SFR_FRAME_TRACE")!=nullptr;
    if(frame_trace)
        std::cerr << "FRAME_DRAW lr=0x" << std::hex << ctx.lr << " ps=0x" << ps_object << std::dec
                  << " t0=" << draw.shared.texture_2d[0] << " count=" << count << " blend=" << draw.blend.blendEnabled
                  << " mask=" << int(draw.write_mask) << '\n';
    if(draw.shared.texture_2d[0]) ++frame_textured_draws;
    static std::set<std::array<uint32_t,4>> seen;
    if(seen.insert({vs_object,ps_object,draw.shared.texture_2d[0],primitive}).second)
        std::cerr << "NATIVE_DRAW source=0x" << std::hex << source << std::dec << " primitive=" << primitive
                  << " count=" << count << " stride=" << stride
                  << " vs=0x" << std::hex << vs_object << " ps=0x" << ps_object << " texture0=" << std::dec
                  << draw.shared.texture_2d[0] << " textures1..3=" << draw.shared.texture_2d[1] << ','
                  << draw.shared.texture_2d[2] << ',' << draw.shared.texture_2d[3] << " write_mask=0x" << std::hex << int(draw.write_mask)
                  << " booleans=0x" << draw.shared.booleans << std::dec << " depth=" << draw.depth_enabled
                  << " blend=" << draw.blend.blendEnabled << " alpha_test=" << alpha_test
                  << " lr=0x" << std::hex << ctx.lr << std::dec << '\n';
}

// Guest vertex data, read in place: native_draw only reads it.
static std::span<const uint8_t> guest_bytes(uint32_t address, uint64_t size) {
    auto& memory=*sfr::active_memory;
    if(size>0x100000) throw sfr::RuntimeStop("native-draw",address,"vertex data is too large");
    memory.check(address,size);
    return {memory.base()+address,size_t(size)};
}

// DrawVerticesUP(device, primitive, vertex count, data, stride).
SFR_HOOK(sub_824F5288) {
    sfr::enter_function(ctx,"sub_824F5288",0x824F5288);
    const uint32_t count=ctx.r5.u32, stride=ctx.r7.u32;
    native_draw(ctx,0x824F5288,ctx.r3.u32,ctx.r4.u32,count,guest_bytes(ctx.r6.u32,uint64_t(count)*stride),stride);
}

// Stream 0 as SetStreamSource stored it: vertex fetch constant in slot 95
// (device +0x778: physical address | 3, then endian and size in bytes) and
// the stride in dwords at +12704.
struct Stream0 { uint32_t physical, size, stride; };
static Stream0 stream0(uint32_t device) {
    auto& memory=*sfr::active_memory;
    const uint32_t fetch0=memory.load<uint32_t>(uint64_t(device)+0x778), fetch1=memory.load<uint32_t>(uint64_t(device)+0x77C);
    const uint32_t stride=uint32_t(memory.load<uint8_t>(uint64_t(device)+12704))*4;
    if((fetch0&3)!=3) throw sfr::RuntimeStop("native-draw",fetch0,"stream 0 has no vertex fetch constant");
    // Word 1: endian (bits 0..1, 2 = 8-in-32 as swap_words assumes) and the size.
    if((fetch1&3)!=2) throw sfr::RuntimeStop("native-draw",fetch1,"unsupported vertex buffer endianness");
    if(!stride) throw sfr::RuntimeStop("native-draw",device,"stream 0 has no stride");
    return {fetch0&0x1FFFFFFCu,fetch1&0x03FFFFFCu,stride};
}

// DrawVertices(device, primitive, start vertex, vertex count) from stream 0.
// SetStreamSource stored stream 0's vertex fetch constant (fetch slot 95,
// device +0x778: physical address | 3, then the byte size) and its stride
// in dwords at +12704; the draw reads the buffer through that constant
// (word 0: byte address | type 3).
SFR_HOOK(sub_824F52D0) {
    sfr::enter_function(ctx,"sub_824F52D0",0x824F52D0);
    auto& memory=*sfr::active_memory;
    const uint32_t device=ctx.r3.u32, primitive=ctx.r4.u32, start=ctx.r5.u32, count=ctx.r6.u32;
    const auto stream=stream0(device);
    if(uint64_t(start)+count>stream.size/stream.stride)
        throw sfr::RuntimeStop("native-draw",start,"draw exceeds the stream 0 vertex buffer");
    const uint64_t bytes=uint64_t(count)*stream.stride;
    const uint32_t data=sfr::NativeRenderer::guest_address(memory,stream.physical+start*stream.stride,bytes);
    native_draw(ctx,0x824F52D0,device,primitive,count,guest_bytes(data,bytes),stream.stride,{},0,
                stream.physical+start*stream.stride);
    static const bool movie_trace=std::getenv("SFR_MOVIE_TRACE")!=nullptr;
    if(movie_trace) {
        static uint32_t traced=0;
        if(traced++%15==0)
            std::cerr << "MOVIE_DRAW y=0x" << std::hex << (graphics().texture_fetch(0)[1]&0xFFFFF000u)
                      << " u=0x" << (graphics().texture_fetch(1)[1]&0xFFFFF000u) << std::dec << '\n';
    }
}

// DrawIndexedVertices(device, primitive, base vertex, start index, index
// count) from stream 0 and the index buffer SetIndices stored at +12612
// (Common bit 31: 32-bit indices; +24: address). The vertices are gathered
// in index order; a strip cut by primitive restart becomes a triangle list.
// Gathering copies one vertex at a time. With the stride known at compile
// time each copy is a couple of moves; with a run-time size it is a call into
// the generic memcpy, and a race frame makes a million of those, which the
// host profile shows as its single largest cost (docs/performance.md).
template<size_t Stride>
static void gather_stride(uint8_t* destination,const uint8_t* source,const uint32_t* order,size_t count) {
    for(size_t i=0;i<count;++i)
        std::memcpy(destination+i*Stride,source+size_t(order[i])*Stride,Stride);
}

static void gather_vertices(uint8_t* destination,const uint8_t* source,const uint32_t* order,size_t count,
                            uint32_t stride) {
    switch(stride) {
    case 8:  gather_stride<8>(destination,source,order,count);  return;
    case 12: gather_stride<12>(destination,source,order,count); return;
    case 16: gather_stride<16>(destination,source,order,count); return;
    case 20: gather_stride<20>(destination,source,order,count); return;
    case 24: gather_stride<24>(destination,source,order,count); return;
    case 28: gather_stride<28>(destination,source,order,count); return;
    case 32: gather_stride<32>(destination,source,order,count); return;
    case 36: gather_stride<36>(destination,source,order,count); return;
    case 40: gather_stride<40>(destination,source,order,count); return;
    case 44: gather_stride<44>(destination,source,order,count); return;
    case 48: gather_stride<48>(destination,source,order,count); return;
    default: break;
    }
    for(size_t i=0;i<count;++i)
        std::memcpy(destination+i*stride,source+size_t(order[i])*stride,stride);
}

SFR_HOOK(sub_824F56E8) {
    sfr::enter_function(ctx,"sub_824F56E8",0x824F56E8);
    if(skip_draws() || !rendering_this_frame()) return;
    auto& memory=*sfr::active_memory;
    const uint32_t device=ctx.r3.u32, base_vertex=ctx.r5.u32, start=ctx.r6.u32, count=ctx.r7.u32;
    uint32_t primitive=ctx.r4.u32;
    const uint32_t buffer=memory.load<uint32_t>(uint64_t(device)+12612);
    if(!buffer) throw sfr::RuntimeStop("native-draw",device,"indexed draw without an index buffer");
    const bool wide=(memory.load<uint32_t>(buffer)&0x80000000u)!=0;
    const uint32_t address=memory.load<uint32_t>(uint64_t(buffer)+24);
    // Physical address as the original computes it (the 0xE0000000 view is 4 KiB ahead).
    const uint32_t index_physical=(address&0x1FFFFFFFu)+(address>=0xE0000000u?0x1000u:0u);
    const uint32_t index_size=wide?4:2;
    if(!count || count>0x40000) throw sfr::RuntimeStop("native-draw",count,"unsupported index count");
    const uint32_t indices=sfr::NativeRenderer::guest_address(memory,index_physical+start*index_size,uint64_t(count)*index_size);
    const auto stream=stream0(device);
    const uint32_t vertex_count=stream.size/stream.stride;
    if(frame_streams.emplace(stream.physical,stream.size).second) frame_stream_bytes+=stream.size;
    const uint32_t restart=wide?0xFFFFFFFFu:0xFFFFu;
    const bool restart_enabled=graphics().render_state().primitive_restart_enabled.value_or(false);
    // One range check for the whole index buffer, then plain big-endian reads:
    // a per-access check for each of a million indices a frame costs more than
    // reading them (docs/performance.md).
    memory.check(indices,uint64_t(count)*index_size);
    const uint8_t* index_bytes=memory.base()+indices;
    frame_indices+=count;
    const auto index_start=std::chrono::steady_clock::now();
    const uint64_t index_cycles=thread_cycles();
    static thread_local std::vector<uint32_t> order;
    order.resize(count);
    const sfr::IndexScan scan=sfr::decode_indices(index_bytes,count,wide,base_vertex,restart_enabled,order.data());
    const bool cut=scan.restart;
    const uint32_t lowest=scan.lowest, highest=scan.highest;
    if(lowest<=highest && highest>=vertex_count)
        throw sfr::RuntimeStop("native-draw",highest,"index exceeds the stream 0 vertex buffer");
    frame_index_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-index_start).count();
    frame_index_cycles+=thread_cycles()-index_cycles;
    if(lowest>highest) return;  // restart indices only
    // Only the vertices the indices reach are located and checked: the title
    // binds streams of megabytes and draws small pieces of them, and checking
    // the whole stream for every draw walked hundreds of page flags each time.
    const uint64_t used=uint64_t(highest-lowest+1)*stream.stride;
    const uint32_t used_address=sfr::NativeRenderer::guest_address(memory,stream.physical+lowest*stream.stride,used);
    memory.check(used_address,used);
    const uint8_t* const used_bytes=memory.base()+used_address;
    // Uploading the block of the stream the indices reach, and letting the GPU
    // do the indexing, replaces one small copy per vertex with one large one.
    // A draw whose indices are scattered over far more of the stream than it
    // uses keeps the gather, which copies less.
    if(!cut) {
        const uint64_t block=uint64_t(highest-lowest+1);
        if(block<=4*order.size() && used<=0x800000) {
            const auto copy_start=std::chrono::steady_clock::now();
            const uint64_t copy_cycles=thread_cycles();
            // Read in place (native_draw swaps it into the upload ring). The
            // indices keep their stream numbering; the base vertex location
            // moves the block back to the start.
            const std::span<const uint8_t> window(used_bytes,size_t(used));
            frame_gather_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-copy_start).count();
            frame_gather_cycles+=thread_cycles()-copy_cycles;
            native_draw(ctx,0x824F56E8,device,primitive,uint32_t(block),window,stream.stride,order,-int32_t(lowest),
                        stream.physical+lowest*stream.stride);
            return;
        }
    }
    if(cut) {
        const auto cut_start=std::chrono::steady_clock::now();
        if(primitive!=6) throw sfr::RuntimeStop("native-draw",primitive,"primitive restart outside a triangle strip");
        // Each strip segment as triangles, alternating winding like the strip.
        std::vector<uint32_t> list;
        for(size_t first=0;first<order.size();) {
            size_t end=first;
            while(end<order.size() && order[end]!=restart) ++end;
            for(size_t k=first;k+2<end;++k) {
                const bool odd=((k-first)&1)!=0;
                list.insert(list.end(),{order[odd?k+1:k],order[odd?k:k+1],order[k+2]});
            }
            first=end+1;
        }
        order=std::move(list);
        primitive=4;
        frame_cut_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cut_start).count();
    }
    if(order.empty()) return;
    // The vertices are gathered in index order rather than uploaded whole with
    // an index buffer: the title binds one large stream and draws small pieces
    // of it, so uploading the stream for every draw is far more data (measured
    // in docs/performance.md).
    const auto gather_start=std::chrono::steady_clock::now();
    for(auto& vertex:order) vertex-=lowest;  // numbered from the checked range
    const std::span<uint8_t> gathered=byte_scratch(order.size()*stream.stride);
    gather_vertices(gathered.data(),used_bytes,order.data(),order.size(),stream.stride);
    frame_gather_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-gather_start).count();
    native_draw(ctx,0x824F56E8,device,primitive,uint32_t(order.size()),gathered,stream.stride);
}

// InsertCallback(device, type, callback, context): the original writes a
// packet the GPU turns into an interrupt that runs callback(context) once the
// preceding commands complete. Native draws are recorded (their data copied)
// immediately and complete with the frame, so the callback runs at present.
SFR_HOOK(sub_824F8AD8) {
    sfr::enter_function(ctx,"sub_824F8AD8",0x824F8AD8);
    if(ctx.r3.u32!=sfr::GuestGraphics::device_address)
        throw sfr::RuntimeStop("native-callback",ctx.r3.u32,"callback on a non-native device");
    static uint32_t reported=0;
    if(reported++<4)
        std::cerr << "NATIVE_GPU_CALLBACK type=0x" << std::hex << ctx.r4.u32 << " callback=0x" << ctx.r5.u32
                  << " context=0x" << ctx.r6.u32 << " lr=0x" << ctx.lr << std::dec << '\n';
    frame_callbacks.emplace_back(ctx.r5.u32,ctx.r6.u32);
}

SFR_HOOK(sub_824F6DC8) {
    sfr::enter_function(ctx,"sub_824F6DC8",0x824F6DC8);
    if(!rendering_this_frame()) return;
    // RT0 as SetRenderTarget left it: the native colour surface, or one of the
    // title's own while SFR_ALLOW_RENDER_TARGETS aliases them to it.
    const uint32_t target=sfr::active_memory->load<uint32_t>(uint64_t(ctx.r3.u32)+0x3148);
    const bool foreign=target!=sfr::GuestGraphics::color_handle;
    // A clear belongs to the pass that follows it (see native_draw).
    static const bool skip_foreign=[]{ const char* t=std::getenv("SFR_SKIP_FOREIGN_TARGETS"); return t && *t!='0'; }();
    const bool submitted=foreign && sfr::GuestGraphics::foreign_render_targets && skip_foreign ? false
        : graphics().clear(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,
                           ctx.r7.u32,float(ctx.f1.f64),ctx.r9.u32);
    if(sfr::graphics_trace()) std::cerr << "NATIVE_CLEAR source=0x824f6dc8 device=0x" << std::hex << ctx.r3.u32
              << " flags=0x" << ctx.r6.u32 << " argb=0x" << ctx.r7.u32
              << " target=0x" << target << std::dec
              << " depth=" << ctx.f1.f64 << " stencil=" << ctx.r9.u32
              << " submitted=" << submitted << '\n';
}

SFR_HOOK(sub_824E96C8) {
    sfr::enter_function(ctx,"sub_824E96C8",0x824E96C8);
    if(sfr::active_memory) {
        sfr::active_memory->check(ctx.r4.u32,24);
        if(sfr::graphics_trace()) {
            std::cerr << "NATIVE_VIEWPORT_REQUEST address=0x" << std::hex << ctx.r4.u32 << " words=";
            for(uint32_t offset=0;offset<24;offset+=4)
                std::cerr << (offset?",":"") << sfr::active_memory->load<uint32_t>(uint64_t(ctx.r4.u32)+offset);
            std::cerr << std::dec << '\n';
        }
    }
    const bool updated=graphics().set_viewport(ctx.r3.u32,ctx.r4.u32);
    if(!sfr::graphics_trace()) return;
    const auto& v=graphics().presentation().raster_state().viewport();
    std::cerr << "NATIVE_VIEWPORT source=0x824e96c8 x=" << v.x << " y=" << v.y
              << " width=" << v.width << " height=" << v.height
              << " min_depth=" << v.minDepth << " max_depth=" << v.maxDepth << " updated=" << updated;
    if(sfr::active_memory) {
        auto& memory=*sfr::active_memory;
        const uint32_t target=memory.load<uint32_t>(uint64_t(ctx.r3.u32)+0x3148);
        std::cerr << " target=0x" << std::hex << target
                  << std::dec << " back_buffer=" << memory.load<uint32_t>(uint64_t(ctx.r3.u32)+0x35BC)
                  << "x" << memory.load<uint32_t>(uint64_t(ctx.r3.u32)+0x35C0);
        // One of the title's own surfaces carries a texture fetch constant at
        // +28, which names the size the viewport is in.
        if(target && target!=sfr::GuestGraphics::color_handle) try {
            sfr::FetchWords words{};
            for(size_t i=0;i<words.size();++i) words[i]=memory.load<uint32_t>(uint64_t(target)+28+i*4);
            const auto fetch=sfr::decode_texture_fetch(words);
            std::cerr << " surface=" << fetch.width << "x" << fetch.height << " format=" << fetch.format;
        } catch(const sfr::RuntimeStop&) {
            std::cerr << " surface=unreadable";
        }
    }
    std::cerr << '\n';
}

SFR_HOOK(sub_824E8C70) {
    sfr::enter_function(ctx,"sub_824E8C70",0x824E8C70);
    graphics().set_scissor(ctx.r3.u32,ctx.r4.u32);
    std::cerr << "NATIVE_SCISSOR source=0x824e8c70 rectangle=0x" << std::hex << ctx.r4.u32 << std::dec << '\n';
}

SFR_HOOK(sub_824E96B8) {
    sfr::enter_function(ctx,"sub_824E96B8",0x824E96B8);
    graphics().set_scissor_enabled(ctx.r3.u32,ctx.r4.u32);
    std::cerr << "NATIVE_SCISSOR_ENABLE source=0x824e96b8 value=" << ctx.r4.u32 << '\n';
}

SFR_HOOK(sub_824E9218) {
    sfr::enter_function(ctx,"sub_824E9218",0x824E9218);
    graphics().set_blend_control(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32);
    // The original void setter has no return value. State is retained for
    // future PSO construction; the original setter submits no GPU commands.
    if(sfr::graphics_trace()) std::cerr << "NATIVE_BLEND_CONTROL source=0x824e9218 target=" << ctx.r4.u32
              << " packed=0x" << std::hex << ctx.r5.u32 << std::dec
              << " state=retained-for-pipeline\n";
}

// SetTexture(device, slot, texture, dirty mask): the native setter performs the
// original fetch-constant merge; host resources are created when a draw uses them.
SFR_HOOK(sub_824F4220) {
    sfr::enter_function(ctx,"sub_824F4220",0x824F4220);
    graphics().set_texture(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u64);
    if(!sfr::graphics_trace()) return;
    std::cerr << "NATIVE_TEXTURE_BIND source=0x824f4220 slot=" << ctx.r4.u32
              << " texture=0x" << std::hex << graphics().texture_binding(ctx.r4.u32)
              << " mask=0x" << ctx.r6.u64 << std::dec;
    if(!ctx.r5.u32) {
        std::cerr << " lr=0x" << std::hex << ctx.lr << std::dec << " state=unbound\n";
        return;
    }
    const auto words=graphics().texture_fetch(ctx.r4.u32);
    const auto f=sfr::decode_texture_fetch(words);
    std::cerr << " fetch=" << std::hex;
    for(size_t i=0;i<words.size();++i) std::cerr << (i?",":"") << words[i];
    std::cerr << std::dec << " type=" << f.type << " format=" << f.format << " endian=" << f.endian
              << " dimension=" << static_cast<int>(f.dimension) << " size=" << f.width << 'x' << f.height << 'x' << f.depth
              << " tiled=" << f.tiled << " pitch=" << f.pitch << " base=0x" << std::hex << f.base_address
              << " mips=0x" << f.mip_address << std::dec << " levels=" << f.min_mip << ".." << f.max_mip
              << " packed=" << f.packed_mips << " lr=0x" << std::hex << ctx.lr << std::dec
              << " state=fetch-retained\n";
}

SFR_HOOK(sub_824E7F68) {
    sfr::enter_function(ctx,"sub_824E7F68",0x824E7F68);
    graphics().set_primitive_restart(ctx.r3.u32,ctx.r4.u32);
    std::cerr << "NATIVE_PRIMITIVE_RESTART source=0x824e7f68 value=0x" << std::hex << ctx.r4.u32
              << std::dec << " enabled=" << *graphics().render_state().primitive_restart_enabled
              << " retained=1\n";
}

SFR_HOOK(sub_824ED770) {
    sfr::enter_function(ctx,"sub_824ED770",0x824ED770);
    create_shader(ctx,base,sfr::ShaderStage::vertex,__imp__sub_824ED770);
}
SFR_HOOK(sub_824E6A08) { set_render_state(ctx,"sub_824E6A08",0x824E6A08,sfr::RenderState::alpha_test_enable); }
SFR_HOOK(sub_824E6EC8) { set_render_state(ctx,"sub_824E6EC8",0x824E6EC8,sfr::RenderState::alpha_function); }
SFR_HOOK(sub_824E6E68) { set_render_state(ctx,"sub_824E6E68",0x824E6E68,sfr::RenderState::alpha_reference); }
SFR_HOOK(sub_824E70D0) { set_render_state(ctx,"sub_824E70D0",0x824E70D0,sfr::RenderState::depth_enable); }
SFR_HOOK(sub_824E7140) { set_render_state(ctx,"sub_824E7140",0x824E7140,sfr::RenderState::depth_function); }
SFR_HOOK(sub_824E7110) { set_render_state(ctx,"sub_824E7110",0x824E7110,sfr::RenderState::depth_write); }
SFR_HOOK(sub_824E69A8) { set_render_state(ctx,"sub_824E69A8",0x824E69A8,sfr::RenderState::cull_mode); }
SFR_HOOK(sub_824E6A40) { set_blend_request(ctx,"sub_824E6A40",0x824E6A40,sfr::BlendRequest::enable); }
SFR_HOOK(sub_824E6B60) { set_blend_request(ctx,"sub_824E6B60",0x824E6B60,sfr::BlendRequest::source); }
SFR_HOOK(sub_824E6BF0) { set_blend_request(ctx,"sub_824E6BF0",0x824E6BF0,sfr::BlendRequest::destination); }
SFR_HOOK(sub_824E6AD0) { set_blend_request(ctx,"sub_824E6AD0",0x824E6AD0,sfr::BlendRequest::operation); }
SFR_HOOK(sub_824E8248) { set_sampler_filter(ctx,"sub_824E8248",0x824E8248,sfr::SamplerFilter::minification); }
SFR_HOOK(sub_824E83F0) { set_sampler_filter(ctx,"sub_824E83F0",0x824E83F0,sfr::SamplerFilter::magnification); }
SFR_HOOK(sub_824ED588) {
    sfr::enter_function(ctx,"sub_824ED588",0x824ED588);
    create_shader(ctx,base,sfr::ShaderStage::pixel,__imp__sub_824ED588);
}

// D3DDevice_Resolve(device, flags, source rectangle, destination texture, ...):
// the title copies what it rendered (flags bits 0..2 name the source: 0..3 a
// render target, 4 the depth surface) into a texture it samples afterwards.
// The native backend renders into its own framebuffer and has no offscreen
// targets yet, so nothing is copied; with SFR_ALLOW_RENDER_TARGETS=1 this
// reports what a race resolves instead of stopping.
SFR_HOOK(sub_824FAB08) {
    sfr::enter_function(ctx,"sub_824FAB08",0x824FAB08);
    if(!rendering_this_frame()) return;
    auto& memory=*sfr::active_memory;
    const uint32_t flags=ctx.r4.u32, destination=ctx.r6.u32;
    uint32_t destination_base=0,width=0,height=0,format=0;
    if(destination) {
        try {
            sfr::FetchWords words{};
            for(size_t i=0;i<words.size();++i) words[i]=memory.load<uint32_t>(uint64_t(destination)+28+i*4);
            const auto fetch=sfr::decode_texture_fetch(words);
            // The destination surface names its memory through one of the
            // mapped views; a draw samples the same memory by its physical
            // address, which is what the resolved copy has to be keyed on.
            destination_base=fetch.base_address;
            if(destination_base>=0xE0000000u) destination_base=destination_base-0xE0000000u+0x1000u;
            else if(destination_base>=0xA0000000u) destination_base&=0x1FFFFFFFu;
            width=fetch.width; height=fetch.height; format=fetch.format;
        } catch(const sfr::RuntimeStop&) {}
    }
    // Sources 0..3 are render targets: the native framebuffer holds what was
    // drawn, so a copy of it becomes the texture at the destination address.
    // Source 4 is the depth surface (shadow maps), which the native backend
    // cannot copy out yet.
    const bool copied=(flags&7)<4 && destination_base!=0;
    if(copied) graphics().renderer().adopt_resolved_target(destination_base);
    // SFR_RESOLVE_DUMP=<file.bmp>: what the framebuffer held at the first few
    // colour resolves (shows whether the scene was drawn into it).
    if(copied) {
        static uint32_t dumps=0;
        // SFR_RESOLVE_DUMP_AFTER=N waits for the Nth present, so a race's
        // resolves are dumped rather than the title's first few.
        static const uint32_t after=[]{ const char* t=std::getenv("SFR_RESOLVE_DUMP_AFTER"); return t?uint32_t(std::strtoul(t,nullptr,10)):0u; }();
        if(const char* pattern=std::getenv("SFR_RESOLVE_DUMP"); pattern && dumps<6 && sfr::present_count>=after) {
            char path[1024];
            std::snprintf(path,sizeof path,pattern,int(dumps++));
            write_framebuffer(path);
            std::cerr << "NATIVE_RESOLVE_DUMP path=" << path << " base=0x" << std::hex << destination_base
                      << std::dec << '\n';
        }
    }
    static std::set<uint64_t> reported;
    const uint64_t key=uint64_t(flags)<<32 | destination_base;
    if(reported.size()<64 && reported.insert(key).second)
        std::cerr << "NATIVE_RESOLVE flags=0x" << std::hex << flags << " source=" << std::dec << (flags&7)
                  << " destination=0x" << std::hex << destination << " base=0x" << destination_base << std::dec
                  << " size=" << width << 'x' << height << " format=" << format
                  << " lr=0x" << std::hex << ctx.lr << std::dec
                  << (copied ? " state=framebuffer-copied\n" : " state=not-copied\n");
    ctx.r3.u64=0;
}
