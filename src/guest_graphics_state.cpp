#include "guest_graphics.h"
#include "guest_memory.h"
#include "native_presentation.h"
#include <plume_render_interface.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace sfr {
namespace {
[[noreturn]] void unsupported(uint32_t address, const char* detail) {
    throw RuntimeStop("native-graphics-state", address, detail);
}
void check_device(GuestMemory& memory, bool created, uint32_t device) {
    if (!created || device != GuestGraphics::device_address)
        unsupported(device,"unknown native device for raster state");
    if (memory.load<uint8_t>(device+0x2ABC)&0x30)
        unsupported(device+0x2ABC,"special raster rectangle mode is unsupported");
}
int32_t integer(float value) {
    if (!std::isfinite(value) || double(value)<INT32_MIN || double(value)>INT32_MAX)
        unsupported(0,"raster coordinate is not a supported finite integer");
    return static_cast<int32_t>(value);
}
plume::RenderViewport read_viewport(GuestMemory& memory,uint32_t device) {
    std::array<float,6> values{};
    for(size_t i=0;i<values.size();++i)
        values[i]=std::bit_cast<float>(memory.load<uint32_t>(device+0x3218+i*4));
    return {values[0],values[1],values[2],values[3],values[4],values[5]};
}
std::array<int32_t,4> read_rectangle(GuestMemory& memory,uint32_t address) {
    memory.check(address,16);
    std::array<int32_t,4> result{};
    for(size_t i=0;i<result.size();++i)
        result[i]=std::bit_cast<int32_t>(memory.load<uint32_t>(uint64_t(address)+i*4));
    return result;
}
struct ScissorUpdate {
    plume::RenderRect native;
    uint32_t top_left, bottom_right;
};
ScissorUpdate scissor_update(GuestMemory& memory,uint32_t device,
                             const plume::RenderViewport& viewport,
                             const std::array<int32_t,4>& rectangle,uint32_t enabled,
                             uint32_t width,uint32_t height) {
    int64_t left=integer(viewport.x),top=integer(viewport.y);
    int64_t right=left+integer(viewport.width),bottom=top+integer(viewport.height);
    if(right<INT32_MIN || right>INT32_MAX || bottom<INT32_MIN || bottom>INT32_MAX)
        unsupported(device,"signed scissor endpoint overflow is unsupported");
    if(enabled) {
        left=std::max<int64_t>(left,rectangle[0]); top=std::max<int64_t>(top,rectangle[1]);
        right=std::min<int64_t>(right,rectangle[2]); bottom=std::min<int64_t>(bottom,rectangle[3]);
    }
    // Preserve the two flag bits which the original rlwimi instructions leave.
    const uint32_t top_left=(memory.load<uint32_t>(device+0x28C4)&0x80008000u) |
        ((uint32_t(top)&0x7FFFu)<<16) | (uint32_t(left)&0x7FFFu);
    const uint32_t bottom_right=(memory.load<uint32_t>(device+0x28C8)&0x80008000u) |
        ((uint32_t(bottom)&0x7FFFu)<<16) | (uint32_t(right)&0x7FFFu);
    left=std::clamp<int64_t>(left,0,width); right=std::clamp<int64_t>(right,0,width);
    top=std::clamp<int64_t>(top,0,height); bottom=std::clamp<int64_t>(bottom,0,height);
    if(left>=right || top>=bottom) left=top=right=bottom=0;
    return {{int32_t(left),int32_t(top),int32_t(right),int32_t(bottom)},top_left,bottom_right};
}
void check_scissor_writes(GuestMemory& memory,uint32_t device) {
    memory.check_write(device+0x3234,16);
    memory.check_write(device+0x28C4,8);
}
void write_scissor(GuestMemory& memory,uint32_t device,const std::array<int32_t,4>& rectangle,
                   const ScissorUpdate& update) {
    for(size_t i=0;i<rectangle.size();++i)
        memory.store<uint32_t>(device+0x3234+i*4,std::bit_cast<uint32_t>(rectangle[i]));
    memory.store<uint32_t>(device+0x28C4,update.top_left);
    memory.store<uint32_t>(device+0x28C8,update.bottom_right);
}
void apply(NativePresentation& presentation,uint32_t device,
            const plume::RenderViewport& viewport,const plume::RenderRect& scissor) {
    try { presentation.set_raster_state(viewport,scissor); }
    catch(const std::exception& error) {
        throw RuntimeStop("native-graphics-state",device,error.what());
    }
}
}

bool GuestGraphics::set_viewport(uint32_t device,uint32_t descriptor) {
    check_device(memory_,created(),device);
    memory_.check(descriptor,24);
    plume::RenderViewport viewport{
        float(memory_.load<uint32_t>(descriptor)),float(memory_.load<uint32_t>(uint64_t(descriptor)+4)),
        float(memory_.load<uint32_t>(uint64_t(descriptor)+8)),float(memory_.load<uint32_t>(uint64_t(descriptor)+12)),
        std::bit_cast<float>(memory_.load<uint32_t>(uint64_t(descriptor)+16)),
        std::bit_cast<float>(memory_.load<uint32_t>(uint64_t(descriptor)+20))};
    uint32_t attachment=memory_.load<uint32_t>(device+0x3148);
    if(!attachment) attachment=memory_.load<uint32_t>(device+0x3158);
    if(!attachment) return false;
    if(attachment!=color_handle && attachment!=depth_handle && !foreign_render_targets)
        unsupported(attachment,"unknown viewport attachment");
    auto& native=presentation();
    const int64_t x=integer(viewport.x),y=integer(viewport.y);
    const int64_t right=x+integer(viewport.width),bottom=y+integer(viewport.height);
    if(right>INT32_MAX || bottom>INT32_MAX)
        unsupported(descriptor,"signed viewport endpoint overflow is unsupported");
    int64_t width=std::min<int64_t>(native.width(),right)-x;
    int64_t height=std::min<int64_t>(native.height(),bottom)-y;
    if(width<0 || height<0) width=height=0;
    viewport.width=float(width); viewport.height=float(height);
    const auto rectangle=read_rectangle(memory_,device+0x3234);
    const auto update=scissor_update(memory_,device,viewport,rectangle,
        memory_.load<uint32_t>(device+0x2F00),native.width(),native.height());
    memory_.check_write(device+0x3218,28);
    memory_.check_write(device+0x2908,24);
    memory_.check_write(device+16,8);
    check_scissor_writes(memory_,device);
    const uint64_t dirty=memory_.load<uint64_t>(device+16)|0x07E00000u;
    apply(native,device,viewport,update.native);
    const std::array<float,7> fields{viewport.x,viewport.y,viewport.width,viewport.height,
        viewport.minDepth,viewport.maxDepth,0};
    for(size_t i=0;i<fields.size();++i)
        memory_.store<uint32_t>(device+0x3218+i*4,std::bit_cast<uint32_t>(fields[i]));
    write_scissor(memory_,device,rectangle,update);
    const float half_width=viewport.width*0.5f,half_height=viewport.height*0.5f;
    const std::array<float,6> transform{half_width,half_width+viewport.x,-half_height,
        half_height+viewport.y,viewport.maxDepth-viewport.minDepth,viewport.minDepth};
    for(size_t i=0;i<transform.size();++i)
        memory_.store<uint32_t>(device+0x2908+i*4,std::bit_cast<uint32_t>(transform[i]));
    memory_.store<uint64_t>(device+16,dirty);
    return true;
}

void GuestGraphics::set_scissor(uint32_t device,uint32_t rectangle_address) {
    check_device(memory_,created(),device);
    const auto rectangle=read_rectangle(memory_,rectangle_address);
    const auto viewport=read_viewport(memory_,device);
    auto& native=presentation();
    const auto update=scissor_update(memory_,device,viewport,rectangle,
        memory_.load<uint32_t>(device+0x2F00),native.width(),native.height());
    check_scissor_writes(memory_,device);
    apply(native,device,viewport,update.native);
    write_scissor(memory_,device,rectangle,update);
}

void GuestGraphics::set_scissor_enabled(uint32_t device,uint32_t enabled) {
    check_device(memory_,created(),device);
    const auto viewport=read_viewport(memory_,device);
    const auto rectangle=read_rectangle(memory_,device+0x3234);
    auto& native=presentation();
    const auto update=scissor_update(memory_,device,viewport,rectangle,enabled,native.width(),native.height());
    memory_.check_write(device+0x2F00,4);
    check_scissor_writes(memory_,device);
    apply(native,device,viewport,update.native);
    memory_.store<uint32_t>(device+0x2F00,enabled);
    write_scissor(memory_,device,rectangle,update);
}
}
