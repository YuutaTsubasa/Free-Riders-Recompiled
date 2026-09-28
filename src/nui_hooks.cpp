#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "camera_player.h"
#include "kinect_sensor.h"
#include "camera_debug.h"
#include "camera_input.h"
#include "nui_skeleton.h"
#include "nui_player_routing.h"
#include "nui_menu_progress.h"
#include "nui_speech.h"
#include "voice_commands.h"
#include "local_profile.h"
#include "touch_controls.h"
#include <bit>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <initializer_list>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <string_view>

// Kinect emulation at the NUI API the title links statically. Without a
// sensor the original NuiInitialize fails and the title never leaves its
// start screen, which only answers Kinect gestures. These replace the five
// entry points the title's Kinect manager uses (identified from its calls:
// 822130D0 initializes and enables tracking with its Event_NuiGetSkeleton,
// the 824395E8 thread waits on that event and fetches frames, 824387E8
// shuts down). Other library functions keep their original bodies and fail
// as before, since the library's own state stays uninitialized.

namespace {
sfr::NuiSkeletonEmulation skeleton;
// SFR_CAMERA=motion: the first player's body comes from a webcam instead of
// the pad. Started at the first frame the title asks for, because the camera
// takes a moment to open and nothing should wait for it.
std::unique_ptr<sfr::CameraPlayer> camera_player;
std::unique_ptr<sfr::CameraDebugWindow> camera_debug;
bool camera_started = false;
sfr::SkeletonJoints camera_joints{};
bool camera_has_joints = false;
// SFR_CAMERA=kinect: a real sensor tracks the players, as on the console.
// Its skeletons go to the title untouched, and the race reads them through
// the title's own detectors instead of the pad (nui_body_from_sensor).
std::unique_ptr<sfr::KinectSensor> kinect;
std::atomic<bool> sensor_body{false};
std::atomic<uint64_t> kinect_generation{0};  // kinect_frame_generation
sfr::KinectFrame kinect_frame;
// A real Kinect's skeletons reach the title as the sensor gives them: in the
// sensor's own six slots, under its own tracking ids, so the depth image's
// player index (slot + 1) marks the same people and a person who steps back
// in is a new skeleton the title identifies again, as on the console. Each
// id's identity is what NuiIdentityIdentify answered for it; unidentified
// (-1) until then. Written by the identity hook, read by the skeleton hook.
std::mutex kinect_identity_lock;
std::unordered_map<uint32_t,uint32_t> kinect_identities;  // sensor tracking id -> enrollment
sfr::CameraInputSelection camera_selection;
std::atomic<bool> camera_input_active{false};
std::atomic<uint64_t> camera_pose_counter{0};
// A second Kinect player, driven by the second pad. The frame carries six
// skeleton slots and the title reads them all, so a player appears simply by
// filling another one; it is identified separately (tracking id 2).
sfr::NuiSkeletonEmulation second_skeleton;
sfr::NuiPlayerRouting player_routing;
bool second_present = false;
sfr::NuiPadEdges edges;
uint16_t pressed=0;  // buttons newly pressed at the last input update
uint32_t frame_number = 0;
const auto started = std::chrono::steady_clock::now();
}

namespace sfr {
bool camera_motion_active() { return camera_input_active.load(std::memory_order_relaxed); }
uint64_t camera_pose_generation() { return camera_pose_counter.load(std::memory_order_relaxed); }
uint64_t camera_motion_clock_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

namespace {
// The title's frame step in seconds ([[83E516A0]+24]+40), which the menus add
// to a delayed action's elapsed time.
float frame_step(sfr::GuestMemory& memory) {
    const uint32_t clock=memory.load<uint32_t>(0x83E516A0);
    if(!clock) return 0.0f;
    return std::bit_cast<float>(memory.load<uint32_t>(memory.load<uint32_t>(clock+24)+40));
}

// Calls a guest function with up to four arguments and returns r3.
uint32_t call_guest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                    uint32_t r3, uint32_t r4=0, uint32_t r5=0, uint32_t r6=0) {
    PPCContext saved=ctx;
    ctx.r3.u64=r3; ctx.r4.u64=r4; ctx.r5.u64=r5; ctx.r6.u64=r6;
    function(ctx,base);
    const uint32_t result=ctx.r3.u32;
    ctx=saved;
    return result;
}

// The menu buttons of every page of the menu manager (+80..+84: 8-byte
// entries, page first; page +300..+304: 8-byte entries, button first).
template<class F> void for_each_menu_button(sfr::GuestMemory& memory, uint32_t manager, F visit) {
    const uint32_t pages=memory.load<uint32_t>(manager+80), pages_end=memory.load<uint32_t>(manager+84);
    for(uint32_t entry=pages; entry<pages_end && entry-pages<8*64; entry+=8) {
        const uint32_t page=memory.load<uint32_t>(entry);
        if(!page) continue;
        const uint32_t buttons=memory.load<uint32_t>(page+300), buttons_end=memory.load<uint32_t>(page+304);
        for(uint32_t slot=buttons; slot<buttons_end && slot-buttons<8*128; slot+=8)
            if(const uint32_t button=memory.load<uint32_t>(slot)) if(!visit(button)) return;
    }
}
uint32_t menu_manager=0;        // last manager whose update ran
uint64_t menu_manager_frame=0;  // input update count at that time
uint64_t input_frames=0;
}


bool sfr::nui_body_from_sensor() { return sensor_body.load(std::memory_order_relaxed); }
uint64_t sfr::kinect_frame_generation() { return kinect_generation.load(std::memory_order_relaxed); }

// NuiInitialize(flags, ?)
SFR_HOOK(sub_8276FD88) {
    sfr::enter_function(ctx,"sub_8276FD88",0x8276FD88);
    std::cerr << "NUI_INITIALIZE flags=0x" << std::hex << ctx.r3.u32 << " argument=0x" << ctx.r4.u32
              << std::dec << " result=0 backend=emulated-skeleton\n";
    ctx.r3.u64=0;
}

// NuiShutdown()
SFR_HOOK(sub_8276DF30) {
    sfr::enter_function(ctx,"sub_8276DF30",0x8276DF30);
    sfr::stop_nui_skeleton_events();
    ctx.r3.u64=0;
}

// NuiSkeletonTrackingEnable(next frame event, flags): the event is signaled
// at the sensor's 30 Hz.
SFR_HOOK(sub_82770668) {
    sfr::enter_function(ctx,"sub_82770668",0x82770668);
    std::cerr << "NUI_SKELETON_TRACKING_ENABLE event=0x" << std::hex << ctx.r3.u32 << " flags=0x" << ctx.r4.u32
              << std::dec << '\n';
    if(ctx.r3.u32) sfr::start_nui_skeleton_events(ctx.r3.u32);
    ctx.r3.u64=0;
}

// NuiSkeletonTrackingDisable()
SFR_HOOK(sub_8276FEE0) {
    sfr::enter_function(ctx,"sub_8276FEE0",0x8276FEE0);
    sfr::stop_nui_skeleton_events();
    ctx.r3.u64=0;
}

// The Kinect's cameras, as the title's image stream object (vtable
// 821A8768, built by 82437F38) uses them: it opens a stream, then every
// fourth frame (82438578) asks NuiImageStreamGetNextFrame for a frame with a
// 10 ms wait, copies the frame's texture (NUI_IMAGE_FRAME +20) into its own
// with 82438328 -- the depth coloured by player index for the screen, and
// the raw depth and player index into its buffer (+72) for the depth view's
// silhouettes (82439530), which steer the race and more -- and releases it.
// The library is not initialized here, so the originals fail and the object
// is thrown away. With a real Kinect (unless SFR_KINECT_DEPTH=0) these give it the
// sensor's frames instead: type 0 depth and player index at resolution 1
// (320x240), type 1 colour at resolution 2 (640x480), in a texture made the
// way the object makes its own, so everything after is the title's code.
namespace {
struct ImageStream {
    uint32_t type = 0, resolution = 0, width = 0, height = 0, bytes_per_pixel = 0, format = 0;
    uint32_t texture = 0, frame = 0;  // guest: the frame's texture, and NUI_IMAGE_FRAME + locked rect
    sfr::KinectImage image;           // the last one handed over
};
constexpr uint32_t image_stream_handle = 0x4E554900u;  // | type
ImageStream image_streams[2];

bool kinect_images_wanted() {
    static const bool wanted=[] {
        const char* camera=std::getenv("SFR_CAMERA");
        const char* depth=std::getenv("SFR_KINECT_DEPTH");
        return camera && std::string_view(camera)=="kinect" && !(depth && *depth=='0');
    }();
    return wanted;
}

ImageStream* image_stream(uint32_t handle) {
    if((handle&~1u)!=image_stream_handle) return nullptr;
    ImageStream& stream=image_streams[handle&1u];
    return stream.texture ? &stream : nullptr;
}

// Calls a guest function with up to eight arguments and returns r3.
uint32_t call_guest8(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                     std::initializer_list<uint32_t> arguments) {
    PPCContext saved=ctx;
    PPCRegister* registers[]={&ctx.r3,&ctx.r4,&ctx.r5,&ctx.r6,&ctx.r7,&ctx.r8,&ctx.r9,&ctx.r10};
    size_t i=0;
    for(const uint32_t value:arguments) registers[i++]->u64=value;
    function(ctx,base);
    const uint32_t result=ctx.r3.u32;
    ctx=saved;
    return result;
}
}

// NuiImageStreamOpen(image type, resolution, frame flags, frame limit, next
// frame event, stream handle out).
PPC_FUNC_IMPL(__imp__sub_82768C40);
SFR_HOOK(sub_82768C40) {
    sfr::enter_function(ctx,"sub_82768C40",0x82768C40);
    const uint32_t type=ctx.r3.u32,resolution=ctx.r4.u32,flags=ctx.r5.u32,limit=ctx.r6.u32,event=ctx.r7.u32,
                   handle=ctx.r8.u32;
    const bool ours=kinect_images_wanted() && handle && ((type==0 && resolution==1) || (type==1 && resolution==2));
    if(ours) {
        auto& memory=*sfr::active_memory;
        ImageStream& stream=image_streams[type];
        if(!stream.texture) {
            // The object's own formats and sizes (82437F38).
            stream.type=type; stream.resolution=resolution;
            stream.width=type==0?320:640; stream.height=type==0?240:480;
            stream.bytes_per_pixel=type==0?2:4;
            stream.format=type==0?0x28280044u:0x28280086u;
            // The frame and the locked rect it is filled through, from the
            // title's allocator, as the object's own memory is.
            const uint32_t heap=memory.load<uint32_t>(0x83E5160C);
            PPCContext saved=ctx;
            ctx.r3.u64=heap; ctx.r4.u64=64; ctx.r5.u64=16;
            sfr::call_indirect(ctx,base,memory.load<uint32_t>(memory.load<uint32_t>(heap)+16));
            stream.frame=ctx.r3.u32;
            ctx=saved;
            stream.texture=stream.frame?call_guest8(ctx,base,sub_824F3EA0,
                {stream.width,stream.height,1,1,0,stream.format,1,3}):0;
            if(stream.frame) for(uint32_t i=0;i<64;i+=4) memory.store<uint32_t>(stream.frame+i,0);
        }
        if(stream.texture) {
            memory.store<uint32_t>(handle,image_stream_handle|type);
            ctx.r3.u64=0;
        } else ctx.r3.u64=0x8007000Eu;  // E_OUTOFMEMORY
    } else __imp__sub_82768C40(ctx,base);
    std::cerr<<"NUI_IMAGE_STREAM_OPEN type="<<type<<" resolution="<<resolution<<" flags=0x"<<std::hex<<flags
             <<" limit="<<std::dec<<limit<<" event=0x"<<std::hex<<event<<" result=0x"<<ctx.r3.u32<<std::dec
             <<" backend="<<(ours?"kinect":"original");
    if(ours) std::cerr<<" texture=0x"<<std::hex<<image_streams[type].texture<<std::dec;
    std::cerr<<'\n';
}

// NuiImageStreamGetNextFrame(stream, wait ms, frame out): the sensor's
// newest image, written into the stream's texture as the console's would be
// (big-endian pixels: 16-bit depth and player index; colour as X8R8G8B8).
PPC_FUNC_IMPL(__imp__sub_82767148);
SFR_HOOK(sub_82767148) {
    sfr::enter_function(ctx,"sub_82767148",0x82767148);
    ImageStream* stream=image_stream(ctx.r3.u32);
    if(!stream) { __imp__sub_82767148(ctx,base); return; }
    const uint32_t out=ctx.r5.u32;
    if(!out) { ctx.r3.u64=0x80004003u; return; }  // E_POINTER
    const auto kind=stream->type==0?sfr::KinectImageKind::depth_and_player:sfr::KinectImageKind::colour;
    // Every five seconds, how many frames of each camera the title asked for
    // and got (the depth ones make the race's lean).
    static uint32_t asked[2]{},given[2]{};
    static auto reported=std::chrono::steady_clock::now();
    ++asked[stream->type&1];
    if(const auto now=std::chrono::steady_clock::now(); now-reported>=std::chrono::seconds(5)) {
        std::cerr<<"NUI_IMAGE_FRAMES depth="<<given[0]<<'/'<<asked[0]<<" colour="<<given[1]<<'/'<<asked[1]<<'\n';
        asked[0]=asked[1]=given[0]=given[1]=0;
        reported=now;
    }
    if(!kinect || !kinect->image(kind,stream->image) || stream->image.width!=stream->width ||
       stream->image.height!=stream->height || stream->image.bytes_per_pixel!=stream->bytes_per_pixel) {
        ctx.r3.u64=0x83010001u;  // E_NUI_FRAME_NO_DATA: the title tries again later
        return;
    }
    ++given[stream->type&1];
    auto& memory=*sfr::active_memory;
    const uint32_t rect=stream->frame+32;  // D3DLOCKED_RECT: pitch, bits
    call_guest8(ctx,base,sub_824F3CF0,{stream->texture,0,rect,0,0});
    const uint32_t pitch=memory.load<uint32_t>(rect),bits=memory.load<uint32_t>(rect+4);
    if(bits && pitch>=stream->width*stream->bytes_per_pixel) {
        const uint8_t* pixel=stream->image.pixels.data();
        for(uint32_t y=0;y<stream->height;++y) {
            const uint32_t row=bits+y*pitch;
            if(stream->type==0) {
                // Depth and player index as the sensor gave them: the index is
                // the skeleton slot + 1, and the skeletons keep their slots.
                for(uint32_t x=0;x<stream->width;++x,pixel+=2)
                    memory.store<uint16_t>(row+x*2,uint16_t(pixel[0]|pixel[1]<<8));
            } else {
                for(uint32_t x=0;x<stream->width;++x,pixel+=4)
                    memory.store<uint32_t>(row+x*4,0xFF000000u|uint32_t(pixel[2])<<16|uint32_t(pixel[1])<<8|pixel[0]);
            }
        }
    }
    call_guest8(ctx,base,sub_824F22A0,{stream->texture,0});
    // NUI_IMAGE_FRAME: time stamp, frame number, type, resolution, texture.
    const uint64_t milliseconds=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    memory.store<uint32_t>(stream->frame,uint32_t(milliseconds>>32));
    memory.store<uint32_t>(stream->frame+4,uint32_t(milliseconds));
    memory.store<uint32_t>(stream->frame+8,uint32_t(stream->image.number));
    memory.store<uint32_t>(stream->frame+12,stream->type);
    memory.store<uint32_t>(stream->frame+16,stream->resolution);
    memory.store<uint32_t>(stream->frame+20,stream->texture);
    memory.store<uint32_t>(out,stream->frame);
    static uint32_t described=0;
    if(described++<4)
        std::cerr<<"NUI_IMAGE_FRAME type="<<stream->type<<" number="<<stream->image.number<<" pitch="<<pitch
                 <<" bits=0x"<<std::hex<<bits<<std::dec<<'\n';
    ctx.r3.u64=0;
}

// Texture LockRect (texture, level, locked rect, rect, flags). A lock with
// D3DLOCK_READONLY (0x10) answers on the console with the texture's cached
// physical view (0xC0000000 up, 0x20000000 below the 0xE0000000 one). Only
// the 0xE0000000 view exists here (physical_memory.h), so such a lock is
// made without the flag: the same memory, where it is mapped. The Kinect's
// image stream object is what reads textures this way -- the frame it is
// given, and its own copies (8243EF28, for the camera image) -- and nothing
// did before its streams opened.
PPC_FUNC_IMPL(__imp__sub_824F3CF0);
SFR_HOOK(sub_824F3CF0) {
    sfr::enter_function(ctx,"sub_824F3CF0",0x824F3CF0);
    if(ctx.r7.u32&0x10) {
        static uint32_t noted=0;
        if(noted++<4) std::cerr<<"NATIVE_TEXTURE_LOCK_READONLY texture=0x"<<std::hex<<ctx.r3.u32<<" flags=0x"<<ctx.r7.u32
                               <<" lr=0x"<<ctx.lr<<std::dec<<" view=0xE0000000\n";
        ctx.r7.u64&=~uint64_t(0x10);
    }
    __imp__sub_824F3CF0(ctx,base);
}

// NuiImageStreamReleaseFrame(stream, frame): the stream keeps its frame.
PPC_FUNC_IMPL(__imp__sub_82767458);
SFR_HOOK(sub_82767458) {
    sfr::enter_function(ctx,"sub_82767458",0x82767458);
    if(!image_stream(ctx.r3.u32)) { __imp__sub_82767458(ctx,base); return; }
    ctx.r3.u64=0;
}

// One of a real Kinect's skeletons in its own slot of the frame
// (NUI_SKELETON_DATA: state, tracking id, enrollment, user index, position,
// 20 joints, their states).
void write_kinect_body(sfr::GuestMemory& memory, uint32_t frame, const sfr::KinectBody& body, uint32_t enrollment) {
    const uint64_t data=uint64_t(frame)+sfr::nui_skeleton_data_offset+uint64_t(body.sensor_index)*sfr::nui_skeleton_data_size;
    const auto vector=[&](uint64_t address,const std::array<float,3>& v) {
        for(uint32_t i=0;i<3;++i) memory.store<uint32_t>(address+4*i,std::bit_cast<uint32_t>(v[i]));
        memory.store<uint32_t>(address+12,std::bit_cast<uint32_t>(1.0f));
    };
    memory.store<uint32_t>(data,sfr::nui_tracked);
    memory.store<uint32_t>(data+4,body.tracking_id);
    memory.store<uint32_t>(data+8,enrollment);
    memory.store<uint32_t>(data+12,body.sensor_index);
    vector(data+16,body.position);
    for(uint32_t j=0;j<sfr::nui_joint_count;++j) {
        vector(data+32+j*16,body.joints[j]);
        memory.store<uint32_t>(data+352+j*4,std::min(body.joint_states[j],sfr::nui_tracked));
    }
}

// NuiSkeletonGetNextFrame(timeout ms, frame): one emulated player.
SFR_HOOK(sub_827707B0) {
    sfr::enter_function(ctx,"sub_827707B0",0x827707B0);
    const uint32_t frame=ctx.r4.u32;
    if(!frame) { ctx.r3.u64=0x80004003u; return; }  // E_POINTER, as the original
    // [83E52F8C] is the race flag: the hands leave the menu cursor pose.
    const bool racing=sfr::active_memory->load<uint32_t>(0x83E52F8C) != 0;
    sfr::set_touch_racing(racing);
    if(!camera_started) {
        camera_started=true;
        const char* const choice=std::getenv("SFR_CAMERA");
        if(choice && std::string_view(choice)=="kinect") {
            std::string why;
            kinect=sfr::KinectSensor::open(&why);
            sensor_body.store(kinect!=nullptr);
            // Without a sensor the pad's emulated player carries on, so a
            // Kinect left unplugged does not leave the title unplayable.
            std::cerr << "NATIVE_KINECT started=" << (kinect?1:0);
            if(kinect) std::cerr << " model=" << kinect->model();
            else std::cerr << " reason=" << why << " fallback=pad";
            std::cerr << '\n';
        } else {
            camera_debug=sfr::CameraDebugWindow::start();
            camera_player=sfr::CameraPlayer::start();
        }
    }
    if(kinect) {
        // Both players are whoever the sensor sees; nobody in front of it is
        // an empty frame, which the title answers the way it did on the
        // console (asking the player to step in). The pads keep their voice
        // commands and menu buttons (the input update below).
        auto& memory=*sfr::active_memory;
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started);
        // Turned once per new frame: the last one is kept as it was turned.
        if(kinect->next(kinect_frame)) {
            // Levelling turns the bodies, which the console never did: it is
            // for the skeleton-only fallback (SFR_KINECT_DEPTH=0), and
            // SFR_KINECT_LEVEL=1 or 0 chooses either way.
            static const bool level=[]{
                const char* t=std::getenv("SFR_KINECT_LEVEL");
                if(t && *t) return *t!='0';
                const char* depth=std::getenv("SFR_KINECT_DEPTH");
                return depth && *depth=='0';
            }();
            const float gravity_y=kinect_frame.gravity[1];
            const float tilt=level?sfr::kinect_level(kinect_frame):0.0f;
            const uint64_t generation=kinect_generation.fetch_add(1,std::memory_order_relaxed)+1;
            // Every three seconds, what reaches the title: the tilt taken
            // out and the first body's centre line and hands, with the hands'
            // tracking states (2 tracked, 1 inferred, 0 not).
            if(generation%90==1) {
                std::ostringstream line;
                line<<"NATIVE_KINECT_BODY frame="<<generation<<" gravity_y="<<gravity_y<<" level="<<tilt
                    <<" bodies="<<kinect_frame.bodies.size();
                if(!kinect_frame.bodies.empty()) {
                    const auto& body=kinect_frame.bodies.front();
                    const auto point=[&](const char* name,uint32_t joint) {
                        const auto& p=body.joints[joint];
                        line<<' '<<name<<'='<<p[0]<<','<<p[1]<<','<<p[2];
                    };
                    point("hip",sfr::nui_joint::hip_center);
                    point("shoulders",sfr::nui_joint::shoulder_center);
                    point("head",sfr::nui_joint::head);
                    point("hand_l",sfr::nui_joint::hand_left);
                    point("hand_r",sfr::nui_joint::hand_right);
                    line<<" hand_states="<<body.joint_states[sfr::nui_joint::hand_left]<<','
                        <<body.joint_states[sfr::nui_joint::hand_right];
                }
                std::cerr<<line.str()<<'\n';
            }
        }
        sfr::publish_second_player_pad(std::nullopt);
        sfr::NuiSkeletonEmulation::write_header(memory,frame,++frame_number,uint64_t(elapsed.count()));
        sfr::NuiSkeletonEmulation::write_floor(memory,frame,kinect_frame.floor_plane,kinect_frame.gravity);
        {
            std::lock_guard guard(kinect_identity_lock);
            // Ids the sensor no longer tracks are gone: whoever steps in next
            // is a new skeleton.
            std::erase_if(kinect_identities,[&](const auto& entry) {
                return std::none_of(kinect_frame.bodies.begin(),kinect_frame.bodies.end(),
                                    [&](const sfr::KinectBody& body) { return body.tracking_id==entry.first; });
            });
            for(const sfr::KinectBody& body:kinect_frame.bodies) {
                if(body.sensor_index>=6) continue;
                const auto known=kinect_identities.find(body.tracking_id);
                write_kinect_body(memory,frame,body,
                                  known==kinect_identities.end()?sfr::NuiSkeletonEmulation::unidentified:known->second);
            }
        }
        static std::vector<uint32_t> seen;
        for(const sfr::KinectBody& body:kinect_frame.bodies)
            if(std::find(seen.begin(),seen.end(),body.tracking_id)==seen.end())
                std::cerr << "NATIVE_KINECT_ENTER tracking_id=" << body.tracking_id << " slot=" << body.sensor_index << '\n';
        seen.clear();
        for(const sfr::KinectBody& body:kinect_frame.bodies) seen.push_back(body.tracking_id);
        if((kinect_frame.bodies.size()>=2)!=second_present) {
            second_present=kinect_frame.bodies.size()>=2;
            std::cerr << "NUI_SECOND_PLAYER present=" << second_present << " source=kinect\n";
        }
        ctx.r3.u64=0;
        return;
    }
    const bool debug_active=camera_debug && !camera_debug->closed();
    sfr::CameraTrackingStatus camera_status;
    if(camera_player && camera_player->joints(camera_joints,&camera_status)) {
        camera_has_joints=true;
        camera_pose_counter.fetch_add(1,std::memory_order_relaxed);
    }
    const auto first=sfr::nui_gamepad();
    const bool use_camera=camera_selection.update(first,camera_has_joints?camera_status.pose_age_ms:-1,
                                                  sfr::CameraInputSelection::Clock::now());
    const bool was_camera=sfr::camera_motion_active();
    camera_input_active.store(use_camera,std::memory_order_relaxed);
    if(use_camera!=was_camera)
        std::cerr<<"NUI_INPUT_SOURCE player=0 source="<<(use_camera?"camera":"controller")
                 <<" pose_age_ms="<<camera_status.pose_age_ms<<'\n';
    // Camera supplements P1. Each configured controller keeps its player,
    // including while the camera opens, loses tracking, or is overridden.
    const auto second=sfr::nui_second_gamepad(1u, racing);
    const bool was_reversed=player_routing.reversed();
    player_routing.update(*sfr::active_memory,skeleton,second_skeleton,
                          first,second,racing,use_camera,was_camera && !use_camera);
    if(was_reversed!=player_routing.reversed())
        std::cerr << "NUI_PLAYER_ROUTING first_tracking_id=" << (player_routing.reversed()?2:1)
                  << " second_tracking_id=" << (player_routing.reversed()?1:2) << '\n';
    // The race hooks need the same pad, and this is where it is decided
    // which one the second player is holding.
    sfr::publish_second_player_pad(second);
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started);
    auto& memory=*sfr::active_memory;
    sfr::NuiSkeletonEmulation::write_header(memory,frame,++frame_number,uint64_t(elapsed.count()));
    // Submit exactly one source for logical P1; never change slot identities.
    player_routing.write_slots(memory,frame,skeleton,second_skeleton,second.has_value(),
                               use_camera?&camera_joints:nullptr);
    // Numeric-only race diagnostics: the exact submitted P1 joints, never
    // camera pixels. Pair with SFR_RACE_BODY_DUMP to inspect guest processing.
    static const bool race_trace=[] {const char* t=std::getenv("SFR_CAMERA_RACE_TRACE");return t && *t=='1';}();
    if(race_trace && racing && frame_number%5==0) {
        const uint64_t data=uint64_t(frame)+sfr::nui_skeleton_data_offset+
            (player_routing.reversed()?sfr::nui_skeleton_data_size:0);
        std::ostringstream line;
        line<<"CAMERA_RACE_JOINTS frame="<<frame_number<<" source="<<(use_camera?"camera":"controller")
            <<" age_ms="<<camera_status.pose_age_ms;
        for(uint32_t j=0;j<sfr::nui_joint_count;++j) {
            line<<" j"<<j<<'=';
            for(uint32_t axis=0;axis<3;++axis)
                line<<(axis?",":"")<<std::bit_cast<float>(memory.load<uint32_t>(data+32+j*16+axis*4));
        }
        std::cerr<<line.str()<<'\n';
    }
    if(debug_active) {
        // Publish only after the guest frame was written, with those exact joints.
        sfr::CameraDebugFrame snapshot;
        snapshot.game_frame=frame_number;
        snapshot.camera_available=bool(camera_player);
        snapshot.has_pose=true;
        snapshot.controller_active=!use_camera;
        const uint64_t data=uint64_t(frame)+sfr::nui_skeleton_data_offset+
            (player_routing.reversed()?sfr::nui_skeleton_data_size:0);
        for(uint32_t joint=0;joint<sfr::nui_joint_count;++joint)
            for(uint32_t axis=0;axis<3;++axis)
                snapshot.joints[joint][axis]=std::bit_cast<float>(memory.load<uint32_t>(data+32+16*joint+4*axis));
        snapshot.detected=camera_status.detected;
        snapshot.observation_age_ms=camera_status.observation_age_ms;
        snapshot.pose_age_ms=camera_status.pose_age_ms;
        snapshot.submitted=sfr::CameraDebugClock::now();
        camera_debug->publish(snapshot);
    }
    if(second.has_value()!=second_present) {
        second_present=second.has_value();
        std::cerr << "NUI_SECOND_PLAYER present=" << second_present << '\n';
    }
    if(frame_number%300==1) {
        const auto hand=use_camera?camera_joints[sfr::nui_joint::hand_right]:
            (player_routing.reversed()?second_skeleton:skeleton).hand(true);
        std::cerr << "NUI_SKELETON_FRAME number=" << frame_number << " right_hand=" << hand[0] << ',' << hand[1]
                  << ',' << hand[2] << " source=" << (use_camera?"camera":"controller") << '\n';
    }
    ctx.r3.u64=0;
}

// NuiIdentityIdentify(tracking id, flags, callback, context): the title
// identifies each new skeleton before it joins as a player (82437E48). The
// emulated player is the local profile (enrollment 0) when one is signed in,
// so the title plays as that profile and keeps its records; otherwise an
// unenrolled guest. The completion message (id 1, result S_OK, enrollment)
// is delivered to the callback at once and the call reports the pending
// asynchronous operation it started.
SFR_HOOK(sub_82764620) {
    sfr::enter_function(ctx,"sub_82764620",0x82764620);
    const uint32_t tracking_id=ctx.r3.u32, flags=ctx.r4.u32, callback=ctx.r5.u32, context=ctx.r6.u32;
    if(flags) { ctx.r3.u64=0x80070057u; return; }  // E_INVALIDARG, as the original
    auto& memory=*sfr::active_memory;
    const uint32_t message=(ctx.r1.u32-0x400)&~15u;  // below the caller's frame
    memory.check_write(message,32);
    for(uint32_t offset=0;offset<32;offset+=4) memory.store<uint32_t>(uint64_t(message)+offset,0);
    memory.store<uint32_t>(message,1);                         // identity operation complete
    memory.store<uint32_t>(uint64_t(message)+4,tracking_id);
    // Only the first player is the signed-in profile; a second one joins as
    // an unenrolled guest, as a friend standing beside the sensor would.
    bool profile=false;
    if(kinect) {
        // The sensor's own ids: the signed-in profile is whoever is identified
        // while nobody tracked holds it; everyone else is a guest.
        std::lock_guard guard(kinect_identity_lock);
        profile=sfr::profile_for(0)!=nullptr &&
                std::none_of(kinect_identities.begin(),kinect_identities.end(),[&](const auto& entry) {
                    return entry.first!=tracking_id && entry.second==0;
                });
        kinect_identities[tracking_id]=profile?0u:sfr::NuiSkeletonEmulation::guest;
    } else {
        profile=tracking_id<=1 && sfr::profile_for(0)!=nullptr;
        (tracking_id>=2?second_skeleton:skeleton).identify(profile?0u:sfr::NuiSkeletonEmulation::guest);
    }
    const uint32_t enrollment=profile?0u:sfr::NuiSkeletonEmulation::guest;
    memory.store<uint32_t>(uint64_t(message)+12,enrollment);
    std::cerr << "NUI_IDENTITY_IDENTIFY tracking_id=" << tracking_id << " callback=0x" << std::hex << callback
              << " context=0x" << context << std::dec << " result=" << (profile?"profile":"guest") << '\n';
    if(callback) {
        PPCContext saved=ctx;
        ctx.r3.u64=context;
        ctx.r4.u64=message;
        ctx.r1.u64=message-0x100;
        sfr::call_indirect(ctx,base,callback);
        ctx=saved;
    }
    ctx.r3.u64=0x8000000Au;  // E_PENDING
}

PPC_FUNC_IMPL(__imp__sub_82494658);

PPC_FUNC_IMPL(__imp__sub_824920F8);
// The title's START screen does not use every later menu-manager path.
// Observe the completed common input update as well as the menu cursor below.
SFR_HOOK(sub_824920F8) {
    sfr::enter_function(ctx,"sub_824920F8",0x824920F8);
    const uint32_t input=ctx.r3.u32;
    __imp__sub_824920F8(ctx,base);
    static const bool trace=[] {const char* text=std::getenv("SFR_NUI_CURSOR_TRACE");return text && *text=='1';}();
    static uint32_t calls=0;
    if(!trace || ++calls%12!=1) return;
    auto& memory=*sfr::active_memory;
    try {
        const auto word=[&](uint64_t at) {return memory.readable(at,4)?memory.load<uint32_t>(at):0u;};
        const auto number=[&](uint64_t at) {return std::bit_cast<float>(word(at));};
        for(uint32_t player=0;player<2;++player) {
            const uint64_t block=uint64_t(input)+32+2544*player;
            const uint32_t body=word(block+12),data=body?word(uint64_t(body)+768):0;
            std::ostringstream line;
            line<<"NUI_INPUT_TRACE player="<<player<<" frame="<<input_frames
                <<" tracking_id="<<(data?word(uint64_t(data)+4):0)
                <<" body_slot="<<(body?word(uint64_t(body)+784):~0u)
                <<" enabled="<<std::hex<<word(block+4)<<std::dec
                <<" unavailable="<<unsigned(memory.load<uint8_t>(block+984));
            for(uint32_t hand=0;hand<2;++hand) {
                const uint64_t packet=block+16+400*hand;
                line<<(hand?" right[":" left[")<<"flags="<<std::hex<<word(packet)
                    <<" inhibit="<<word(packet+4)<<std::dec
                    <<" angle="<<number(packet+164)<<" raise="<<number(packet+384)
                    <<" lower="<<number(packet+388)<<']';
            }
            if(data) for(const uint32_t joint : {sfr::nui_joint::hand_right,sfr::nui_joint::elbow_right,
                                                sfr::nui_joint::shoulder_right,sfr::nui_joint::hand_left,
                                                sfr::nui_joint::elbow_left,sfr::nui_joint::shoulder_left}) {
                const uint64_t at=uint64_t(data)+32+16*joint;
                line<<" j"<<joint<<'='<<number(at)<<','<<number(at+4)<<','<<number(at+8);
            }
            std::cerr<<line.str()<<'\n';
        }
    } catch(const std::exception& error) {
        std::cerr<<"NUI_INPUT_TRACE unreadable="<<error.what()<<'\n';
    }
}

// Input update (once per frame, input object = [83E52FB8]): after the
// original has published the recognizer's word, a newly pressed pad button
// is heard as a voice command at +5440 with high confidence (+5444 score,
// +5448 level 2). [83E52F8C] is nonzero during a race. The buttons pressed
// since the previous update stay available to the hand-only dialogs below
// until the next update.
SFR_HOOK(sub_82494658) {
    sfr::enter_function(ctx,"sub_82494658",0x82494658);
    const uint32_t input=ctx.r3.u32;
    auto& memory=*sfr::active_memory;
    __imp__sub_82494658(ctx,base);
    const bool racing=memory.load<uint32_t>(0x83E52F8C)!=0;
    pressed=edges.update(sfr::nui_gamepad().buttons);
    ++input_frames;
    uint16_t spoken=pressed;
    // B of a menu with a back button is that button's (824578F0 below).
    if((pressed & sfr::gamepad_button::b) && menu_manager && input_frames-menu_manager_frame<=2) {
        bool back=false;
        for_each_menu_button(memory,menu_manager,[&](uint32_t b) { back=memory.load<uint32_t>(b+288)==31; return !back; });
        if(back) spoken&=~sfr::gamepad_button::b;
    }
    // SFR_SAY="word@present,..." speaks any word of the title's vocabulary at
    // the given present, to find which one a page answers to (the words live
    // in the image around 0x821A84D8: next, restart, replay, mainmenu,
    // courseslc, ruleslc, gearslc, charaslc, playernumslc, missionslc).
    static const auto script=[]{
        std::vector<std::pair<uint32_t,std::string>> said;
        const char* text=std::getenv("SFR_SAY");
        for(std::string_view rest=text?text:""; !rest.empty();) {
            const auto comma=rest.find(',');
            const auto entry=rest.substr(0,comma);
            const auto at=entry.find('@');
            if(at!=std::string_view::npos)
                said.emplace_back(uint32_t(std::strtoul(std::string(entry.substr(at+1)).c_str(),nullptr,10)),
                                  std::string(entry.substr(0,at)));
            rest=comma==std::string_view::npos ? std::string_view{} : rest.substr(comma+1);
        }
        return said;
    }();
    static size_t said_index=0;
    if(said_index<script.size() && sfr::present_count>=script[said_index].first) {
        const auto& entry=script[said_index++];
        memory.check_write(uint64_t(input)+5440,12);
        memory.store<uint32_t>(uint64_t(input)+5440,sfr::NuiSpeechEmulation::say(memory,entry.second));
        memory.store<uint32_t>(uint64_t(input)+5444,0x3F800000u);
        memory.store<uint32_t>(uint64_t(input)+5448,2);
        std::cerr << "NUI_SAID word=" << entry.second << " present=" << sfr::present_count << '\n';
        return;
    }
    // SFR_VOICE=1: the player's own voice, through the host's speech
    // recognizer (a Kinect's microphone array is one to Windows). A phrase
    // heard is the title's word, as a button would be; the pad still works.
    static std::unique_ptr<sfr::VoiceRecognizer> voice=[]() -> std::unique_ptr<sfr::VoiceRecognizer> {
        const char* wanted=std::getenv("SFR_VOICE");
        if(!wanted || !*wanted || *wanted=='0') return nullptr;
        std::string why;
        auto recognizer=sfr::VoiceRecognizer::open(sfr::voice_phrases(),&why);
        if(!recognizer) std::cerr << "NATIVE_VOICE started=0 reason=" << why << '\n';
        return recognizer;
    }();
    if(std::string phrase; voice && voice->next(phrase)) {
        const auto title_word=sfr::title_word_for(phrase,racing);
        if(!title_word.empty()) {
            memory.check_write(uint64_t(input)+5440,12);
            memory.store<uint32_t>(uint64_t(input)+5440,sfr::NuiSpeechEmulation::say(memory,title_word));
            memory.store<uint32_t>(uint64_t(input)+5444,0x3F800000u);
            memory.store<uint32_t>(uint64_t(input)+5448,2);
            std::cerr << "NUI_VOICE heard=\"" << phrase << "\" word=" << title_word << " racing=" << racing << '\n';
            return;
        }
    }
    const auto word=sfr::NuiSpeechEmulation::hear(spoken,racing);
    if(word==sfr::NuiSpeechEmulation::Word::none) return;
    static bool written=false;
    if(!written) { sfr::NuiSpeechEmulation::write_words(memory); written=true; }
    memory.check_write(uint64_t(input)+5440,12);
    memory.store<uint32_t>(uint64_t(input)+5440,sfr::NuiSpeechEmulation::address(word));
    memory.store<uint32_t>(uint64_t(input)+5444,0x3F800000u);  // 1.0f
    memory.store<uint32_t>(uint64_t(input)+5448,2);
    const auto text=sfr::NuiSpeechEmulation::text(word);
    std::cerr << "NUI_SPEECH word=" << std::string(text.begin(),text.end()) << " racing=" << racing << '\n';
}

PPC_FUNC_IMPL(__imp__sub_823EF348);

// Hand-pointer dialog choice (dialog, hand): the slot 0..2 whose button the
// hand held long enough, or 3 for none. Used by the title's Omochao prompt
// (8244F880) and two other dialogs; +124 is the layout, 4 for the two
// buttons in slots 0 (yes) and 1 (no), 3 for a single button in slot 2. The
// original only answers the hand, so a pressed A or B chooses directly, with
// the confirm (16) or cancel (8) sound the original plays through 8222FA18.
SFR_HOOK(sub_823EF348) {
    sfr::enter_function(ctx,"sub_823EF348",0x823EF348);
    auto& memory=*sfr::active_memory;
    const uint32_t dialog=ctx.r3.u32;
    const int slot=ctx.r4.u32 ? sfr::dialog_choice(pressed,memory.load<uint32_t>(uint64_t(dialog)+124)) : -1;
    if(slot<0) { __imp__sub_823EF348(ctx,base); return; }
    pressed=0;  // one choice per press
    PPCContext saved=ctx;
    ctx.r3.u64=memory.load<uint32_t>(0x83E52E18);
    ctx.r4.u64=slot==1 ? 8 : 16;
    ctx.r5.u64=1;
    sub_8222FA18(ctx,base);
    ctx=saved;
    std::cerr << "NUI_DIALOG_CHOICE dialog=0x" << std::hex << dialog << std::dec << " slot=" << slot << '\n';
    ctx.r3.u64=uint64_t(slot);
}

PPC_FUNC_IMPL(__imp__sub_8246A6D0);

// Ring menu step for one player (the object's first word is the player,
// 1-based, 0 for the first): the hand swipe as a signed number of items.
// Its callers (823F3148, 82454360, 824543D0) add both players' steps, so a
// newly pressed D-pad left or right of the first player turns the ring one
// item when the hand did not, once per press.
SFR_HOOK(sub_8246A6D0) {
    sfr::enter_function(ctx,"sub_8246A6D0",0x8246A6D0);
    const uint32_t player=sfr::active_memory->load<uint32_t>(ctx.r3.u32);
    __imp__sub_8246A6D0(ctx,base);
    const int step=sfr::ring_step(pressed);
    if(player>1 || ctx.r3.u32 || !step) return;
    pressed&=~(sfr::gamepad_button::dpad_left|sfr::gamepad_button::dpad_right);
    ctx.r3.s64=step;
}

PPC_FUNC_IMPL(__imp__sub_82452F48);
SFR_HOOK(sub_82452F48) {
    sfr::enter_function(ctx,"sub_82452F48",0x82452F48);
    const uint32_t slot=ctx.r3.u32;
    __imp__sub_82452F48(ctx,base);
    sfr::NuiMenuServiceScope::record_wait(slot,(ctx.r3.u32 & 0xff)!=0);
}

PPC_FUNC_IMPL(__imp__sub_82456700);
SFR_HOOK(sub_82456700) {
    sfr::enter_function(ctx,"sub_82456700",0x82456700);
    // This updates the cursor before checking whether a pending animation
    // blocks its delayed action, so elapsed time alone cannot prove service.
    sfr::NuiMenuServiceScope::record(ctx.r3.u32,ctx.r4.u32);
    __imp__sub_82456700(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_824560A8);
// Observe the game's own hand activation result, logical-player binding and
// cursor state together. Opt-in diagnostics only; no gesture or input changes.
SFR_HOOK(sub_824560A8) {
    sfr::enter_function(ctx,"sub_824560A8",0x824560A8);
    const uint32_t manager=ctx.r3.u32,player=ctx.r4.u32;
    __imp__sub_824560A8(ctx,base);
    static const bool trace=[] {const char* text=std::getenv("SFR_NUI_CURSOR_TRACE");return text && *text=='1';}();
    static uint32_t calls[2]{};
    if(!trace || player>=2 || ++calls[player]%12!=1) return;
    auto& memory=*sfr::active_memory;
    try {
        const auto word=[&](uint64_t at) {return memory.readable(at,4)?memory.load<uint32_t>(at):0u;};
        const auto number=[&](uint64_t at) {return std::bit_cast<float>(word(at));};
        const uint32_t input=word(0x83E52FB8),players=word(0x83E52F88);
        const uint32_t body=players?word(uint64_t(players)+120+4*player):0;
        const uint32_t data=body?word(uint64_t(body)+768):0;
        const uint32_t owner=word(uint64_t(manager)+96+4*player);
        const uint32_t hands=owner?word(uint64_t(owner)+112):0;
        std::ostringstream line;
        line<<"NUI_CURSOR_TRACE player="<<player<<" frame="<<input_frames
            <<" tracking_id="<<(data?word(uint64_t(data)+4):0)
            <<" body_slot="<<(body?word(uint64_t(body)+784):~0u)
            <<" manager="<<std::hex<<manager<<" data="<<data<<std::dec;
        for(uint32_t hand=0;hand<2;++hand) {
            const uint64_t packet=uint64_t(input)+2544*player+400*hand;
            const uint64_t cursor=uint64_t(hands)+116*hand;
            line<<(hand?" right[":" left[")<<"flags="<<std::hex<<(input?word(packet+48):0)<<std::dec
                <<" raise="<<(input?number(packet+432):0)<<" lower="<<(input?number(packet+436):0)
                <<" state="<<(hands?word(cursor+100):~0u)<<" object="<<std::hex<<(hands?word(cursor+8):0)<<std::dec<<']';
        }
        if(data) for(const uint32_t joint : {sfr::nui_joint::hand_right,sfr::nui_joint::shoulder_right,
                                           sfr::nui_joint::hand_left,sfr::nui_joint::shoulder_left,sfr::nui_joint::head}) {
            const uint64_t at=uint64_t(data)+32+16*joint;
            line<<" j"<<joint<<'='<<number(at)<<','<<number(at+4)<<','<<number(at+8);
        }
        std::cerr<<line.str()<<'\n';
    } catch(const std::exception& error) {
        std::cerr<<"NUI_CURSOR_TRACE unreadable="<<error.what()<<'\n';
    }
}

PPC_FUNC_IMPL(__imp__sub_824578F0);

// Menu update (this = manager+36). Buttons carry a type at +288 and flags at
// +492; hands select them by holding. Without a hand, B takes the page's
// back button (type 31: 8245B130 asks whether it may leave, 824603B8 leaves)
// and Y an enabled (+492 bit 0x01000000) shortcut button (types 33..35, the
// parts shop: menu commands 15, 16 and 49 through 82454848, with sound 18).
SFR_HOOK(sub_824578F0) {
    sfr::enter_function(ctx,"sub_824578F0",0x824578F0);
    auto& memory=*sfr::active_memory;
    const uint32_t manager=ctx.r3.u32-36;
    menu_manager=manager;
    menu_manager_frame=input_frames;
    // Follow the title's own active-page lookup (+344 == 6, player from
    // +320 bit 4). Inactive gaps matter even if the allocator reuses a page.
    for (uint32_t player=0; player<2; ++player) {
        const uint32_t page=call_guest(ctx,base,sub_82457348,manager,player);
        bool cursor_pending=false;
        if (page && memory.load<uint8_t>(0x83E515FB)) {
            const uint32_t owner=memory.load<uint32_t>(manager+96+4*player);
            if (owner && memory.readable(uint64_t(owner)+112,4)) {
                const uint32_t hands=memory.load<uint32_t>(uint64_t(owner)+112);
                // 824560A8's right-hand state: 2 hidden, 1 activating, 0 active.
                if (hands && memory.readable(uint64_t(hands)+116+100,4))
                    cursor_pending=memory.load<uint32_t>(uint64_t(hands)+116+100)!=0;
            }
        }
        player_routing.observe_menu_page(player,page,page ? memory.load<uint32_t>(page+336) : 0,cursor_pending);
    }
    // SFR_MENU_DUMP=1 reports each menu page: its buttons as type/flags/kind/state
    // and the player's current page, whenever they change (a debugging aid).
    if(std::getenv("SFR_MENU_DUMP")) {
        static std::string last;
        std::string line;
        for_each_menu_button(memory,manager,[&](uint32_t b) {
            char text[64];
            std::snprintf(text,sizeof text," %u/%08x/%u/%x",memory.load<uint32_t>(b+288),memory.load<uint32_t>(b+492),
                          memory.load<uint32_t>(b+312),memory.load<uint32_t>(b+308));
            line+=text;
            return true;
        });
        // The player's current page (82457348(manager, player)) and its buttons.
        const uint32_t page=call_guest(ctx,base,sub_82457348,manager,0);
        line+=" | page";
        if(page) {
            char head[128];
            std::snprintf(head,sizeof head," @%08x vt=%08x m348=%u m352=%u m1848=%u s128=%08x s136=%08x",page,
                          memory.load<uint32_t>(page),memory.load<uint32_t>(page+348),memory.load<uint32_t>(page+352),
                          memory.load<uint32_t>(page+1848),memory.load<uint32_t>(page+128),memory.load<uint32_t>(page+136));
            line+=head;
            const uint32_t b0=memory.load<uint32_t>(page+300), b1=memory.load<uint32_t>(page+304);
            for(uint32_t slot=b0; slot<b1 && slot-b0<8*64; slot+=8) {
                const uint32_t b=memory.load<uint32_t>(slot);
                if(!b) continue;
                char text[64];
                std::snprintf(text,sizeof text," %u/%08x/%u/%x",memory.load<uint32_t>(b+288),memory.load<uint32_t>(b+492),
                              memory.load<uint32_t>(b+312),memory.load<uint32_t>(b+308));
                line+=text;
            }
        }
        if(line!=last) { last=line; std::cerr << "MENU_BUTTONS manager=0x" << std::hex << manager << std::dec << line << '\n'; }
    }
    namespace button=sfr::gamepad_button;
    if(pressed & button::b) {
        uint32_t back=0;
        for_each_menu_button(memory,manager,[&](uint32_t b) {
            if(memory.load<uint32_t>(b+288)==31) { back=b; return false; }
            return true;
        });
        if(back) {
            pressed&=~button::b;
            const uint32_t page=memory.load<uint32_t>(back+280);
            if(call_guest(ctx,base,sub_8245B130,page)&0xFF) {
                const uint32_t side=(memory.load<uint32_t>(memory.load<uint32_t>(back+284)+320)>>4)&1;
                call_guest(ctx,base,sub_824603B8,page,side);
                std::cerr << "NUI_MENU_BACK button=0x" << std::hex << back << std::dec << '\n';
            }
        }
    }
    // Delayed menu actions live at manager+488+16*player. Some pages
    // stop servicing them; inspect the original update below before deciding
    // whether recovery is needed. SFR_MENU_STALL=0 disables recovery.
    // SFR_MENU_STATE=1: the manager's state ([r3+4]) and the queued action, on
    // every change and every two seconds. The state dispatches as state - 2:
    // 2 and 3 open the pages, 4 runs them (it is where the page update
    // 82456D60 -> 82456700 is called from), 5 builds the page change's fade
    // into +436 and 6 waits for it to reach [[r3+436]+20] == 3.
    static const bool state_trace=[]{ const char* t=std::getenv("SFR_MENU_STATE"); return t && *t!='0'; }();
    // SFR_WATCH_SLOT=<0|1>: watch that player slot's "pending request" word
    // ([r3+1828+88*slot+20], which 82452F48 waits on), so each change reports
    // the function entered right after it.
    static const int watch_slot=[]{ const char* t=std::getenv("SFR_WATCH_SLOT"); return t ? std::atoi(t) : -1; }();
    if(watch_slot>=0 && !sfr::watch_word.load(std::memory_order_relaxed))
        sfr::watch_word.store(ctx.r3.u32+1828+88*uint32_t(watch_slot)+20,std::memory_order_relaxed);
    static uint32_t last_state=~0u;
    const uint32_t state=memory.load<uint32_t>(ctx.r3.u32+4);
    if(state_trace && (input_frames%120==0 || state!=last_state))
    {
        last_state=state;
        const uint32_t waited=memory.load<uint32_t>(ctx.r3.u32+436);
        std::cerr << "NUI_MENU_STATE frame=" << input_frames << " state=" << state
                  << " action=" << memory.load<uint32_t>(manager+488)
                  << " elapsed=" << std::bit_cast<float>(memory.load<uint32_t>(manager+496))
                  << " waited=0x" << std::hex << waited << std::dec
                  << " waited20=" << (waited ? memory.load<uint32_t>(waited+20) : 0u);
        // State 5 asks [[83E5160C]]->vtable[4](100, 16) for the fade object
        // state 6 then waits on.
        const uint32_t fader=memory.load<uint32_t>(0x83E5160C);
        const uint32_t vtable=fader ? memory.load<uint32_t>(fader) : 0;
        std::cerr << std::hex << " fader=0x" << fader << " vtable=0x" << vtable << " method=0x"
                  << (vtable ? memory.load<uint32_t>(vtable+16) : 0u) << std::dec << '\n';
        // State 5 only builds that fade once both player slots (+1828 and
        // +1916, 88 bytes each) have latched their "settled" byte at +59 and
        // the queue at [[83E52FB8]+5128 .. +5132] is empty; any of the three
        // failing leaves the state as it is, for good.
        if(state==5) try {
            const uint32_t queue=memory.load<uint32_t>(0x83E52FB8);
            std::cerr << "NUI_MENU_LEAVE queue=0x" << std::hex << queue << std::dec;
            if(queue)
                std::cerr << " first=" << memory.load<uint32_t>(queue+5128)
                          << " last=" << memory.load<uint32_t>(queue+5132);
            for(uint32_t slot=0; slot<2; ++slot) {
                const uint32_t at=ctx.r3.u32+1828+88*slot;
                std::cerr << " | slot" << slot << " @0x" << std::hex << at << std::dec;
                for(uint32_t word=0; word<22; ++word)
                    std::cerr << ' ' << word*4 << '=' << memory.load<uint32_t>(at+word*4);
            }
            std::cerr << '\n';
        } catch(const std::exception& error) {
            std::cerr << "NUI_MENU_LEAVE unreadable: " << error.what() << '\n';
        }
    }
    if(pressed & button::y) {
        uint32_t command=0;
        for_each_menu_button(memory,manager,[&](uint32_t b) {
            if(memory.load<uint32_t>(b+492)&0x01000000) command=sfr::menu_shortcut_command(memory.load<uint32_t>(b+288));
            return command==0;
        });
        if(command) {
            pressed&=~button::y;
            call_guest(ctx,base,sub_82454848,manager,0,3,command);
            call_guest(ctx,base,sub_8222FA18,memory.load<uint32_t>(0x83E52E18),18,8);
            std::cerr << "NUI_MENU_SHORTCUT command=" << command << '\n';
        }
    }
    const auto actions = [&] {
        sfr::NuiMenuActions result;
        for (uint32_t player=0; player<2; ++player) {
            const uint32_t at=manager+488+16*player;
            result[player]={memory.load<uint32_t>(at),memory.load<uint32_t>(at+8),memory.load<uint32_t>(at+12)};
        }
        return result;
    };
    const auto before=actions();
    uint32_t serviced=0, blocked=0;
    {
        sfr::NuiMenuServiceScope scope(manager);
        __imp__sub_824578F0(ctx,base);
        serviced=scope.serviced();
        blocked=scope.blocked();
    }
    const auto after=actions();
    uint32_t recovery_serviced=0;
    static sfr::NuiMenuProgress progress;
    static bool recovering=false;
    static const bool run_stalled=[]{ const char* t=std::getenv("SFR_MENU_STALL"); return !t || *t!='0'; }();
    recovering=progress.update(manager,before,after,
        [&] { return memory.load<uint8_t>(0x83E515FB)!=0; },
        [&](uint32_t player) {
            if(!recovering && player==0)
                std::cerr << "NUI_MENU_UPDATE_RUN manager=0x" << std::hex << manager << std::dec
                          << " type=" << after[0].type << ',' << after[1].type << '\n';
            call_guest(ctx,base,sub_82456700,manager,player);
            recovery_serviced|=1u<<player;
        },run_stalled,serviced);
    // The original state-4 manager returns before updating either player
    // when one confirmation request is outstanding. Keep the other Gear
    // page responsive without settling that request or advancing the page.
    uint32_t independently_serviced=0;
    const auto gear_service_mask=[&] {
        std::array<uint32_t,2> kinds{};
        uint32_t settled=0;
        for (uint32_t player=0; player<2; ++player) {
            const uint32_t page=call_guest(ctx,base,sub_82457348,manager,player);
            if (page) kinds[player]=memory.load<uint32_t>(page+336);
            if (memory.load<uint8_t>(manager+36+1828+88*player+59)) settled|=1u<<player;
        }
        return sfr::nui_gear_service_mask(
            sfr::second_player_pad().has_value() && memory.load<uint8_t>(0x83E515FB),
            memory.load<uint32_t>(manager+40),memory.load<uint8_t>(manager+124)!=0,
            kinds,settled,blocked,serviced|recovery_serviced|independently_serviced);
    };
    if (blocked) for (uint32_t player=0; player<2; ++player) {
        // The first player's callback may change the page, state or 2P flag.
        if (gear_service_mask() & (1u<<player)) {
            call_guest(ctx,base,sub_82456700,manager,player);
            independently_serviced|=1u<<player;
        }
    }

}

PPC_FUNC_IMPL(__imp__sub_82439530);

// Kinect depth view update on the manager's worker thread: it takes the
// enabled flag (+7756) and the "DepthView" resource (+7736) without a lock,
// and the resource is created on the main thread (82438CC8). Until it is
// there the update has nothing to do; the original would read through the
// null pointer.
SFR_HOOK(sub_82439530) {
    sfr::enter_function(ctx,"sub_82439530",0x82439530);
    if(!sfr::active_memory->load<uint32_t>(uint64_t(ctx.r3.u32)+7736)) {
        static uint32_t skipped=0;
        if(skipped++<4) std::cerr << "NUI_DEPTH_VIEW_SKIPPED manager=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    // Once: what the console's depth stream would feed (the object's size
    // and buffer, and its vtable[0], which fetches a frame).
    static bool described=false;
    if(!described) {
        described=true;
        auto& m=*sfr::active_memory;
        const uint32_t view=m.load<uint32_t>(uint64_t(ctx.r3.u32)+7736),vtable=m.load<uint32_t>(view);
        std::cerr<<"NUI_DEPTH_VIEW object=0x"<<std::hex<<view<<" vtable=0x"<<vtable<<" fetch=0x"
                 <<(vtable?m.load<uint32_t>(vtable):0)<<std::dec<<" open="<<unsigned(m.load<uint8_t>(uint64_t(view)+77))
                 <<" enabled="<<unsigned(m.load<uint8_t>(uint64_t(ctx.r3.u32)+7756))
                 <<" width="<<m.load<uint32_t>(uint64_t(view)+48)<<" height="<<m.load<uint32_t>(uint64_t(view)+52)
                 <<" buffer=0x"<<std::hex<<m.load<uint32_t>(uint64_t(view)+72)<<std::dec<<'\n';
    }
    __imp__sub_82439530(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_82463808);

// "A part is still moving": the menu update (82456700) drops every delayed
// action of every page while this reports one. On the gear parts page the
// page change is such a delayed action, and the report never clears, so the
// page never leaves. The original looks up the part object the scene list
// holds ([83E5312C], entries with +344 == 6 and +336 == 56) and answers from
// its two fields at +364 and +396.
//
// After a second of nothing else happening the answer becomes "no": the
// animation it waits for is the part flying onto the board, and whatever
// keeps it pending here, the page it blocks is the one the player asked for.
SFR_HOOK(sub_82463808) {
    sfr::enter_function(ctx,"sub_82463808",0x82463808);
    const uint32_t manager=ctx.r3.u32;
    __imp__sub_82463808(ctx,base);
    static uint32_t busy=0;
    if(!(ctx.r3.u32&0xFF)) { busy=0; return; }
    if(++busy<120) {
        // The pairs it answers from: the parts state ([manager+56]) keeps a part
        // and its animation at +364/+396 and +400/+432, and a running slide at
        // +392/+428.
        if(busy==1) {
            auto& parts=*sfr::active_memory;
            const uint32_t state=parts.load<uint32_t>(manager+56);
            std::cerr << "NUI_MENU_PART_BUSY manager=0x" << std::hex << manager << " state=0x" << state << std::dec;
            if(state)
                std::cerr << " f364=" << parts.load<uint32_t>(state+364)
                          << " f396=" << parts.load<uint32_t>(state+396)
                          << " f400=" << parts.load<uint32_t>(state+400)
                          << " f432=" << parts.load<uint32_t>(state+432)
                          << " f392=" << parts.load<uint32_t>(state+392)
                          << " f428=" << parts.load<uint32_t>(state+428);
            std::cerr << '\n';
        }
        return;
    }
    if(busy==120) std::cerr << "NUI_MENU_PART_RELEASED after=" << busy << " calls\n";
    ctx.r3.u64=0;
}

PPC_FUNC_IMPL(__imp__sub_824A3398);

// Pool allocation (pool, size, alignment, ?): the menus ask this for the fade
// object a page transition waits on ([[83E5160C]]->vtable[4](100, 16)), and on
// the gear parts page it comes back null, which is why that page never leaves.
// Reports the first failures with the pool's free list head, to tell an
// exhausted pool from a wrong argument.
SFR_HOOK(sub_824A3398) {
    sfr::enter_function(ctx,"sub_824A3398",0x824A3398);
    const uint32_t pool=ctx.r3.u32, size=ctx.r4.u32, alignment=ctx.r5.u32;
    __imp__sub_824A3398(ctx,base);
    if(ctx.r3.u32) return;
    static std::atomic<uint32_t> failures{0};
    if(failures++>=8) return;
    auto& memory=*sfr::active_memory;
    const uint32_t list=memory.load<uint32_t>(pool+36);
    std::cerr << "NUI_POOL_EMPTY pool=0x" << std::hex << pool << " size=" << std::dec << size
              << " alignment=" << alignment << " list=0x" << std::hex << list
              << " head=0x" << (list ? memory.load<uint32_t>(list) : 0u) << std::dec << '\n';
}

PPC_FUNC_IMPL(__imp__sub_8243D1B8);

// The page transition's fade, started only when the object the manager waits
// on exists: reporting its calls says whether that object was never created
// or created and then lost (see docs/pad-menus.md).
SFR_HOOK(sub_8243D1B8) {
    sfr::enter_function(ctx,"sub_8243D1B8",0x8243D1B8);
    static uint32_t calls=0;
    if(calls++<8)
        std::cerr << "NUI_MENU_FADE object=0x" << std::hex << ctx.r3.u32 << " a=" << std::dec << ctx.r4.u32
                  << " b=" << ctx.r5.u32 << " colour=0x" << std::hex << ctx.r6.u32 << std::dec << '\n';
    __imp__sub_8243D1B8(ctx,base);
}
