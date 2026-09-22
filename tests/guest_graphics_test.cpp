#include "guest_graphics.h"
#include "graphics_defaults_fixture.h"
#include "guest_memory.h"
#include "native_graphics.h"
#include "native_presentation.h"
#include "native_raster_state.h"
#include "native_blend_control.h"
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
template<class F> void rejects(F operation) {
    try { operation(); } catch (const sfr::RuntimeStop&) { return; }
    throw std::runtime_error("unsupported guest operation did not stop");
}
constexpr uint32_t params=0x10000000, output=params+124, rects=params+128;
constexpr auto device=sfr::GuestGraphics::device_address;
void setup(sfr::GuestMemory& memory) {
    memory.map(params,256);
    constexpr std::array<uint32_t,31> words{
        19,11,0x18280186,1,0,0,1,0,0,1,0x1A220197,0,0,1,0,0,0x28280106,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    for (size_t i=0;i<words.size();++i) memory.store<uint32_t>(params+i*4,words[i]);
    memory.store<uint32_t>(output,0xDEADBEEF);
    memory.map(0x82AD0000,0x2000);
    for (auto [table,count] : {std::pair{0x82AD0A60u,101u},std::pair{0x82AD0F20u,20u}})
        for (uint32_t i=0;i<count;++i) {
            memory.store<uint32_t>(table+i*12,0x12340000+i);
            memory.store<uint32_t>(table+i*12+4,0x82210000+i*4);
            memory.store<uint32_t>(table+i*12+8,i);
        }
    sfr::test::install_blend_defaults(memory);
    sfr::test::install_sampler_defaults(memory);
}
void pixels(sfr::GuestGraphics& guest, uint32_t inside, uint32_t outside,
            int left=0,int top=0,int right=19,int bottom=11) {
    const auto actual=guest.presentation().readback_color();
    require(actual.size()==19*11*4,"GPU readback extent");
    for (int y=0;y<11;++y) for(int x=0;x<19;++x) {
        const uint32_t expected=(x>=left && x<right && y>=top && y<bottom)?inside:outside;
        const auto offset=(y*19+x)*4;
        for (int c=0;c<4;++c)
            require(actual[offset+c]==uint8_t(expected>>(8*c)),"guest ARGB/rectangle GPU result");
    }
}
void run() {
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    sfr::GuestGraphics guest(memory,graphics);
    setup(memory);
    rejects([&]{guest.create_device(0,2,0,0,params,output);});
    memory.store<uint32_t>(params+8,0);
    rejects([&]{guest.create_device(0,1,0,0,params,output);});
    require(!guest.created() && memory.load<uint32_t>(output)==0xDEADBEEF,"failed profile preserves output");
    require(!graphics.initialized(),"rejected profile initializes no native graphics objects");
    require(memory.available(device,sfr::GuestGraphics::device_size),"rejected profile maps no native device");
    memory.store<uint32_t>(params+8,0x18280186);
    // Unknown requested-state defaults must stop before host/device publication.
    constexpr uint32_t source_default = 0x82AD0A60 + 0x48 * 3 + 8;
    memory.store<uint32_t>(source_default, 2);
    rejects([&]{guest.create_device(0,1,0,0,params,output);});
    require(!graphics.initialized() && !guest.created() && memory.load<uint32_t>(output)==0xDEADBEEF,
            "unknown blend defaults do not publish host or guest device");
    memory.store<uint32_t>(source_default, 1);
    memory.store<uint32_t>(source_default-4, 0x824E6BF0);
    rejects([&]{guest.create_device(0,1,0,0,params,output);});
    require(!graphics.initialized() && !guest.created() && memory.load<uint32_t>(output)==0xDEADBEEF,
            "valid but wrong setter cannot supply the original blend defaults");
    memory.store<uint32_t>(source_default-4, 0x824E6B60);
    constexpr uint32_t sampler_mip_default = 0x82AD0F20 + 6 * 12 + 8;
    memory.store<uint32_t>(sampler_mip_default, 0);
    rejects([&]{guest.create_device(0,1,0,0,params,output);});
    require(!graphics.initialized() && !guest.created() && memory.load<uint32_t>(output)==0xDEADBEEF,
            "unknown sampler defaults do not initialize graphics or publish device");
    memory.store<uint32_t>(sampler_mip_default, 2);
    require(guest.create_device(0,1,0,0,params,output)==0,"native creation succeeds");
    require(guest.created() && memory.load<uint32_t>(output)==device,"creation publishes guest device");
    require(memory.load<uint32_t>(device+0x323C)==65535 && memory.load<uint32_t>(device+0x2F00)==0,"original disabled scissor defaults");
    require(memory.load<uint32_t>(device+0x40)==0x82210000,"original dispatch table retained");
    rejects([&]{memory.load<uint32_t>(sfr::GuestGraphics::color_handle);});
    rejects([&]{memory.load<uint32_t>(sfr::GuestGraphics::color_handle+8);});
    rejects([&]{guest.create_device(0,1,0,0,params,output);});

    // Original packet writers pre-increment device+48 and call the space
    // function once it passes device+56; natively the packets are discarded.
    using G = sfr::GuestGraphics;
    constexpr uint32_t empty = G::command_scratch-4;
    require(memory.load<uint32_t>(device+48)==empty &&
            memory.load<uint32_t>(device+56)==G::command_scratch+G::command_scratch_size-G::command_scratch_margin,
            "scratch command buffer published");
    memory.store<uint32_t>(G::command_scratch+G::command_scratch_size-4,0xC0013F00);
    rejects([&]{memory.store<uint32_t>(G::command_scratch+G::command_scratch_size,0);});
    memory.store<uint32_t>(device+48,G::command_scratch+8);
    uint32_t discarded=0;
    require(guest.discard_commands(device,discarded)==empty && discarded==12 &&
            memory.load<uint32_t>(device+48)==empty,"three stored words are discarded");
    require(guest.discard_commands(device,discarded)==empty && discarded==0,"empty buffer discards nothing");
    memory.store<uint32_t>(device+48,G::command_scratch+G::command_scratch_size);
    rejects([&]{guest.discard_commands(device,discarded);});
    memory.store<uint32_t>(device+48,G::command_scratch+2);
    rejects([&]{guest.discard_commands(device,discarded);});
    rejects([&]{guest.discard_commands(device+4,discarded);});
    memory.store<uint32_t>(device+48,empty);

    // Present resolves the native back buffer (RT0) into the native front buffer.
    guest.present_front_buffer(device);
    rejects([&]{guest.present_front_buffer(device+4);});
    for (uint32_t field : {0x3AC4u,0x3148u,0x3AC0u}) {
        const auto saved=memory.load<uint32_t>(device+field);
        memory.store<uint32_t>(device+field,0x70000000);
        rejects([&]{guest.present_front_buffer(device);});
        memory.store<uint32_t>(device+field,saved);
    }
    guest.present_front_buffer(device);

    require(guest.clear(device,99,0,0x31,0x80402010,0.25f,0xA5),"null rectangle means full clear");
    pixels(guest,0x80402010,0);
    require(!guest.clear(device,0,0xDEADBEEF,1,0,0,0),"non-null zero-count rectangle is no-op");
    rejects([&]{guest.clear(device,1,0xDEADBEEF,1,0,0,0);});
    rejects([&]{guest.clear(device,0,0,0x40,0,0,0);});
    pixels(guest,0x80402010,0);
    // Clip a partly out-of-bounds signed rectangle by both viewport and scissor.
    memory.store<uint32_t>(device+0x3218,std::bit_cast<uint32_t>(2.75f));
    memory.store<uint32_t>(device+0x321C,std::bit_cast<uint32_t>(1.75f));
    memory.store<uint32_t>(device+0x3220,std::bit_cast<uint32_t>(12.75f));
    memory.store<uint32_t>(device+0x3224,std::bit_cast<uint32_t>(8.75f));
    memory.store<uint32_t>(device+0x2F00,1);
    memory.store<uint32_t>(device+0x3234,4);
    memory.store<uint32_t>(device+0x3238,3);
    memory.store<uint32_t>(device+0x323C,17);
    memory.store<uint32_t>(device+0x3240,10);
    memory.store<uint32_t>(rects,uint32_t(-20));
    memory.store<uint32_t>(rects+4,uint32_t(-10));
    memory.store<uint32_t>(rects+8,50);
    memory.store<uint32_t>(rects+12,20);
    require(guest.clear(device,1,rects,1,0xFFA1B2C3,0,0),"rectangle clear submitted");
    pixels(guest,0xFFA1B2C3,0x80402010,4,3,14,9);
    memory.store<uint32_t>(device+0x3148,0);
    require(!guest.clear(device,0,0,0x31,0,0,0),"unbound selected color skips original depth fallback");
    require(guest.clear(device,0,0,0x30,0,0.5f,0x7C),"explicit depth-only clear remains supported");
    pixels(guest,0xFFA1B2C3,0x80402010,4,3,14,9);
    memory.store<uint32_t>(device+0x3148,0xBAD00000);
    rejects([&]{guest.clear(device,0,0,1,0,0,0);});
}
void viewport_state() {
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    sfr::GuestGraphics guest(memory,graphics);
    setup(memory);
    guest.create_device(0,1,0,0,params,output);
    constexpr uint32_t viewport=rects+32;
    auto set_descriptor = [&](uint32_t x,uint32_t y,uint32_t w,uint32_t h,float near_depth,float far_depth) {
        for (auto [i,v] : {std::pair{0u,x},std::pair{1u,y},std::pair{2u,w},std::pair{3u,h}})
            memory.store<uint32_t>(viewport+i*4,v);
        memory.store<uint32_t>(viewport+16,std::bit_cast<uint32_t>(near_depth));
        memory.store<uint32_t>(viewport+20,std::bit_cast<uint32_t>(far_depth));
    };
    auto field = [&](uint32_t offset) { return std::bit_cast<float>(memory.load<uint32_t>(device+offset)); };
    const bool reversed=guest.presentation().raster_state().inverted_depth_supported();
    const float near_depth=reversed?1.0f:0.0f,far_depth=reversed?0.0f:1.0f;
    if(!reversed) {
        set_descriptor(2,1,40,20,1,0);
        rejects([&]{guest.set_viewport(device,viewport);});
        require(field(0x3218)==0 && field(0x3220)==19 && field(0x3228)==0 && field(0x322C)==1,
                "unsupported reversed depth preserves original guest viewport");
    }
    memory.store<uint64_t>(device+16,0x1000000000000000);
    memory.store<uint32_t>(device+0x28C4,0x80008000);
    memory.store<uint32_t>(device+0x28C8,0x80008000);
    set_descriptor(2,1,40,20,near_depth,far_depth);
    require(guest.set_viewport(device,viewport),"original integer viewport updates");
    require(field(0x3218)==2 && field(0x321C)==1 && field(0x3220)==17 && field(0x3224)==10,"viewport clamps to attachment dimensions");
    require(field(0x3228)==near_depth && field(0x322C)==far_depth && memory.load<uint32_t>(device+0x3230)==0,"supported depth range and original zero word retained");
    require(field(0x2908)==8.5f && field(0x290C)==10.5f && field(0x2910)==-5 && field(0x2914)==6,"original viewport XY scale and offset");
    require(field(0x2918)==far_depth-near_depth && field(0x291C)==near_depth,"original depth scale and offset");
    require(memory.load<uint64_t>(device+16)==0x1000000007E00000,"viewport marks exact original dirty bits and preserves others");
    require(memory.load<uint32_t>(device+0x28C4)==0x80018002 && memory.load<uint32_t>(device+0x28C8)==0x800B8013,"disabled scissor packs viewport and preserves flag bits");

    const std::array<uint32_t,6> old_view{memory.load<uint32_t>(device+0x3218),memory.load<uint32_t>(device+0x321C),
        memory.load<uint32_t>(device+0x3220),memory.load<uint32_t>(device+0x3224),memory.load<uint32_t>(device+0x3228),memory.load<uint32_t>(device+0x322C)};
    set_descriptor(0,0,19,11,std::numeric_limits<float>::quiet_NaN(),0);
    rejects([&]{guest.set_viewport(device,viewport);});
    for(size_t i=0;i<old_view.size();++i) require(memory.load<uint32_t>(device+0x3218+i*4)==old_view[i],"invalid depth leaves guest state unchanged");
    set_descriptor(128,0,2147483520u,1,near_depth,far_depth);
    rejects([&]{guest.set_viewport(device,viewport);});
    for(size_t i=0;i<old_view.size();++i) require(memory.load<uint32_t>(device+0x3218+i*4)==old_view[i],"overflowing signed endpoint leaves state unchanged");
    rejects([&]{guest.set_viewport(device,0xDEADBEEF);});
    set_descriptor(0,0,19,11,0,1);
    require(guest.set_viewport(device,viewport),"restore full normal viewport");
    guest.clear(device,0,0,1,0xFF102030,0,0);
    // The setter must influence the actual subsequent native clear clipping.
    set_descriptor(2,1,12,8,near_depth,far_depth);
    guest.set_viewport(device,viewport);
    for (auto [i,v] : {std::pair{0u,4u},std::pair{1u,3u},std::pair{2u,17u},std::pair{3u,10u}})
        memory.store<uint32_t>(rects+i*4,v);
    guest.set_scissor(device,rects);
    require(memory.load<uint32_t>(device+0x2F00)==0,"scissor rectangle setter preserves disabled state");
    guest.set_scissor_enabled(device,1);
    require(memory.load<uint32_t>(device+0x28C4)==0x80038004 && memory.load<uint32_t>(device+0x28C8)==0x8009800E,"enabled scissor packs intersection");
    guest.clear(device,0,0,1,0xFFABCDEF,0,0);
    pixels(guest,0xFFABCDEF,0xFF102030,4,3,14,9);
    guest.set_scissor_enabled(device,0);
    guest.clear(device,0,0,1,0xFF010203,0,0);
    pixels(guest,0xFF010203,0xFF102030,2,1,14,9);

    set_descriptor(20,1,1,4,near_depth,far_depth);
    guest.set_viewport(device,viewport);
    require(field(0x3220)==0 && field(0x3224)==0,"negative clamped extent zeroes both viewport dimensions");
    require(!guest.clear(device,0,0,1,0,0,0),"empty viewport submits no clear");
    memory.store<uint32_t>(device+0x3148,0);
    memory.store<uint32_t>(device+0x3158,0);
    set_descriptor(0,0,19,11,near_depth,far_depth);
    require(!guest.set_viewport(device,viewport),"original no-attachment viewport is no-op");
    require(field(0x3218)==20 && field(0x3220)==0,"no-attachment call retains previous state");
}
void blend_state() {
    sfr::GuestMemory memory;
    sfr::NativeGraphics graphics;
    sfr::GuestGraphics guest(memory,graphics);
    setup(memory);
    rejects([&]{guest.set_blend_control(device,0,0x07010706);});
    rejects([&]{guest.blend_control(0);});
    guest.create_device(0,1,0,0,params,output);
    constexpr std::array<uint32_t,4> offsets{0x2938,0x2958,0x295C,0x2960};
    constexpr std::array<uint64_t,4> dirty_bits{0x20400,0x20004,0x20002,0x20001};
    constexpr uint64_t original_dirty=0x123456789ABC0000;
    for (uint32_t i=0;i<4;++i) {
        require(guest.blend_control(i)==sfr::decode_blend_control(0x00010001),"original default initializes effective copy");
        memory.store<uint32_t>(device+offsets[i],0xA1000000+i);
    }
    rejects([&]{guest.set_blend_control(device+4,0,0x07010706);});
    for (uint32_t i=0;i<4;++i) {
        memory.store<uint64_t>(device+16,original_dirty);
        guest.set_blend_control(device,i,0x07010706);
        require(memory.load<uint32_t>(device+offsets[i])==0x07010706,"blend setter writes original per-target cache");
        require(memory.load<uint64_t>(device+16)==(original_dirty|dirty_bits[i]),"blend setter preserves 64-bit dirty state and ORs exact target bits");
        const auto desc=guest.blend_control(i).description(5);
        require(desc.blendEnabled && desc.srcBlend==plume::RenderBlend::SRC_ALPHA &&
            desc.dstBlend==plume::RenderBlend::INV_SRC_ALPHA && desc.blendOp==plume::RenderBlendOperation::ADD &&
            desc.srcBlendAlpha==plume::RenderBlend::ONE && desc.dstBlendAlpha==plume::RenderBlend::INV_SRC_ALPHA &&
            desc.blendOpAlpha==plume::RenderBlendOperation::ADD && desc.renderTargetWriteMask==5,
            "actual native owner retains decoded color/alpha state with separately supplied mask");
        for(uint32_t j=i+1;j<4;++j) {
            require(guest.blend_control(j)==sfr::decode_blend_control(0x00010001),"other native target retains original copy default");
            require(memory.load<uint32_t>(device+offsets[j])==0xA1000000+j,"other target cache untouched");
        }
    }
    guest.set_blend_control(device,1,0x00010001);
    require(!guest.blend_control(1).blendEnabled && guest.blend_control(0).blendEnabled &&
        guest.blend_control(2).blendEnabled && guest.blend_control(3).blendEnabled,"native targets remain independent");
    memory.store<uint64_t>(device+16,0);
    guest.set_blend_control(device,1,0x00010001);
    require(memory.load<uint64_t>(device+16)==dirty_bits[1],"equal repeated state still marks original dirty bits");
    const auto before=guest.blend_control(0);
    const auto dirty=memory.load<uint64_t>(device+16);
    for(uint32_t index : {4u,UINT32_MAX}) {
        rejects([&]{guest.set_blend_control(device,index,0x07010706);});
        rejects([&]{guest.blend_control(index);});
    }
    for(uint32_t word : {0x07012706u,0x27010706u,0x07010702u,0x070107A6u,0x0701070Eu})
        rejects([&]{guest.set_blend_control(device,0,word);});
    require(guest.blend_control(0)==before && memory.load<uint32_t>(device+offsets[0])==0x07010706 &&
        memory.load<uint64_t>(device+16)==dirty,"invalid state mutates neither owner nor guest cache/dirty");
    const auto reserved=memory.load_reserved_word(params);
    rejects([&]{guest.set_blend_control(device,0,0x00010001);});
    require(memory.has_reservation() && guest.blend_control(0)==before,"rejected update preserves live guest atomic and native state");
    require(memory.store_conditional_word(params,reserved),"rejected setter preserves original reservation success");
    // A late dirty-word guard must be checked before even the earlier cache write.
    memory.add_read_only_word(device+16,[dirty]{return uint32_t(dirty>>32);});
    rejects([&]{guest.set_blend_control(device,0,0x00010001);});
    require(guest.blend_control(0)==before && memory.load<uint32_t>(device+offsets[0])==0x07010706 &&
        memory.load<uint64_t>(device+16)==dirty,"late output guard causes no partial native or guest write");
}
}
int main() {
    try { run(); viewport_state(); blend_state(); std::cout << "guest graphics tests passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
