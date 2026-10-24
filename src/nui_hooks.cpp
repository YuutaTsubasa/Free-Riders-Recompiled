#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "camera_player.h"
#include "kinect_sensor.h"
#include "kinect_race_record.h"
#include "kinect_nui_record.h"
#include "kinect_record_depth.h"
#include "kinect_preview.h"
#include "kinect_tuner.h"
#include "camera_tuner.h"
#include "kinect_stability.h"
#include "window_title.h"
#include "native_input.h"
#include "camera_debug.h"
#include "camera_input.h"
#include "pause_gesture.h"
#include "nui_skeleton.h"
#include "nui_player_routing.h"
#include "nui_menu_progress.h"
#include "nui_speech.h"
#include "nui_analysis_trace.h"
#include "voice_commands.h"
#include "local_profile.h"
#include "touch_controls.h"
#include "title_stats.h"
#include "motion_trace.h"
#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
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
// the title's own depth detectors instead of the pad (nui_body_from_sensor).
std::unique_ptr<sfr::KinectSensor> kinect;
// The menu's Kinect adjustment window over that sensor (kinect_tuner.h).
std::mutex tuner_lock;
std::atomic<uint64_t> tuner_generation{0};  // kinect_tuner_generation
std::unique_ptr<sfr::KinectPreviewWindow> tuner;
// The same over the webcam, when that is what the player uses.
std::unique_ptr<sfr::CameraTunerWindow> camera_tuner;
std::once_flag kinect_start_once;
std::atomic<bool> sensor_body{false};
// When the last depth frame was handed to the game (steady clock, ms): a gap
// of more than half a second is logged once the frames come again, and
// nothing else changes -- the game's own detectors read what it gets.
std::atomic<int64_t> depth_given_ms{INT64_MIN/2};
// How many depth images the game has been handed, and the newest one's
// number: the depth view (82439530) is then known to have run on that image
// (SFR_KINECT_RECORD, record_depth_view).
std::atomic<uint64_t> depth_given_count{0}, depth_given_number{0};
std::atomic<uint64_t> kinect_generation{0};  // kinect_frame_generation
sfr::KinectFrame kinect_frame;
sfr::KinectPlayerSlots kinect_players;
// The Pause Gesture (pause_gesture.h): seen on the skeleton hook,
// heard as the pause command by the next input update.
sfr::PauseGesture pause_gesture=sfr::PauseGesture::from_environment();
std::atomic<bool> pause_requested{false};
// active: a race (or its replay) is running with no menu page or dialog
// up. Anywhere else (the menus, the pause menu itself, story scenes,
// results) the gesture does nothing: there, a second pause would be START
// answering the pause menu, and the menus have nothing for it to open.
// The gauge in the lower left, as the console drew it, once the arm has
// been up for a moment (a pose passed through in play does not flash it).
void show_pause_gesture(float progress,bool fired) {
    sfr::publish_pause_gauge(progress>=0.15f ? progress : 0.0f);
    if(!fired) return;
    pause_requested.store(true,std::memory_order_relaxed);
    std::cerr << "NUI_PAUSE_GESTURE pause\n";
}
void watch_pause_gesture(bool posed,bool active) {
    if(!active) {
        pause_gesture.forget();
        sfr::publish_pause_gauge(0.0f);
        return;
    }
    const bool fired=pause_gesture.update(posed,std::chrono::steady_clock::now());
    show_pause_gesture(pause_gesture.progress(),fired);
}
// A Kinect's people each charge their own gauge (by tracking id): two
// players half holding it in turn do not add up to a pause, and someone the
// sensor loses starts again. The fullest one is shown.
std::unordered_map<uint32_t,sfr::PauseGesture> body_pause_gestures;
void watch_body_pause_gestures(const sfr::KinectFrame& frame,bool active) {
    if(!active) {
        body_pause_gestures.clear();
        sfr::publish_pause_gauge(0.0f);
        return;
    }
    std::erase_if(body_pause_gestures,[&](const auto& entry) {
        return std::none_of(frame.bodies.begin(),frame.bodies.end(),
                            [&](const sfr::KinectBody& body){ return body.tracking_id==entry.first; });
    });
    const auto now=std::chrono::steady_clock::now();
    bool fired=false;
    float progress=0.0f;
    for(const sfr::KinectBody& body:frame.bodies) {
        auto& gesture=body_pause_gestures.try_emplace(body.tracking_id,sfr::PauseGesture::from_environment()).first->second;
        const bool posed=sfr::PauseGesture::in_pose(body.joints,body.joint_states) ||
                         sfr::PauseGesture::stop_sign(body.joints,body.hand_states,body.hand_tips,body.thumbs);
        fired|=gesture.update(posed,now);
        progress=(std::max)(progress,gesture.progress());
    }
    show_pause_gesture(progress,fired);
}
// A real Kinect's skeletons reach the title as the sensor gives them: in the
// sensor's own six slots, under its own tracking ids, so the depth image's
// player index (slot + 1) marks the same people and a person who steps back
// in is a new skeleton the title identifies again, as on the console. Each
// id's identity is what NuiIdentityIdentify answered for it; unidentified
// (-1) until then. Written by the identity hook, read by the skeleton hook.
std::mutex kinect_identity_lock;
std::unordered_map<uint32_t,uint32_t> kinect_identities;  // sensor tracking id -> enrollment
// The motion trace (motion_trace.h) of a real Kinect: what the game is given
// for its first two bodies, numbers only. KINECT_TRACE every third sensor
// frame (ten a second): the hips, the shoulders and the hands in metres
// (camera space after levelling, so +y is up), the hands' tracking states
// (2 tracked, 1 inferred), a v2's hand reading (2 open, 3 a fist, 4 lasso)
// and how far the menu steadying moved each hand (millimetres; 0 in races).
// KINECT_TRACE_JOINTS once a second: all twenty joints of the first body.
// About 2 KB a second.
void trace_kinect_frame(const sfr::KinectFrame& frame,uint64_t generation,int64_t captured_ms,bool racing,float tilt,
                        const std::unordered_map<uint32_t,std::array<float,2>>& steady_mm) {
    if(generation%3!=0) return;
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(3);
    line<<"KINECT_TRACE generation="<<generation<<" t_ms="<<captured_ms<<" racing="<<racing<<" level="<<tilt
        <<" bodies="<<frame.bodies.size();
    const auto point=[&](const char* name,const std::array<float,3>& p) {
        line<<' '<<name<<'='<<p[0]<<','<<p[1]<<','<<p[2];
    };
    uint32_t written=0;
    for(const sfr::KinectBody& body:frame.bodies) {
        if(written++==2) break;
        line<<" | id="<<body.tracking_id<<" slot="<<body.sensor_index;
        point("hip",body.joints[sfr::nui_joint::hip_center]);
        point("sc",body.joints[sfr::nui_joint::shoulder_center]);
        point("sl",body.joints[sfr::nui_joint::shoulder_left]);
        point("sr",body.joints[sfr::nui_joint::shoulder_right]);
        point("hl",body.joints[sfr::nui_joint::hand_left]);
        point("hr",body.joints[sfr::nui_joint::hand_right]);
        line<<" tracked="<<body.joint_states[sfr::nui_joint::hip_center]<<','
            <<body.joint_states[sfr::nui_joint::hand_left]<<','<<body.joint_states[sfr::nui_joint::hand_right]
            <<" hand="<<unsigned(body.hand_states[0])<<','<<unsigned(body.hand_states[1]);
        const auto shift=steady_mm.find(body.tracking_id);
        if(shift!=steady_mm.end()) {
            line.precision(1);
            line<<" steady_mm="<<shift->second[0]<<','<<shift->second[1];
            line.precision(3);
        }
    }
    std::cerr<<line.str()<<'\n';
    if(generation%30==0 && !frame.bodies.empty()) {
        std::ostringstream joints;
        joints.setf(std::ios::fixed);
        joints.precision(3);
        const sfr::KinectBody& body=frame.bodies.front();
        joints<<"KINECT_TRACE_JOINTS generation="<<generation<<" id="<<body.tracking_id;
        for(uint32_t j=0;j<sfr::nui_joint_count;++j)
            joints<<" j"<<j<<'='<<body.joints[j][0]<<','<<body.joints[j][1]<<','<<body.joints[j][2]
                  <<'/'<<body.joint_states[j];
        std::cerr<<joints.str()<<'\n';
    }
}
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
sfr::NuiPadEdges second_edges;
uint16_t second_pressed=0;  // the same for the second player's pad
// Relay Race: frames the racer is still out of view during a swap, the
// tracking id of the one in view (0: the usual 1), and the controller the
// next racer holds. Written by the input update, read by the skeleton hook.
std::atomic<uint32_t> relay_out{0}, relay_tracking_id{0}, relay_next_pad{0};
uint32_t frame_number = 0;
const auto started = std::chrono::steady_clock::now();

// Image streams can be requested before the first skeleton. Resolve the
// backend at either entry point so stream-open success reflects its cameras.
void start_kinect() {
    std::call_once(kinect_start_once,[] {
        std::string why;
        kinect=sfr::KinectSensor::open_selected(sfr::kinect_selected_model(),&why);
        sensor_body.store(kinect!=nullptr,std::memory_order_relaxed);
        std::cerr << "NATIVE_KINECT started=" << (kinect?1:0);
        if(kinect) std::cerr << " model=" << kinect->model() << " depth=" << kinect->has_depth_stream();
        else std::cerr << " reason=" << why << " fallback=pad";
        std::cerr << '\n';
        // No substitute: without a depth stream the game gets none either
        // (NuiImageStreamOpen is left to the library, which fails without a
        // sensor) and its own detectors see nothing.
        if(kinect && !kinect->has_depth_stream())
            std::cerr << "NATIVE_KINECT_DEPTH unavailable model=" << kinect->model() << " reason="
                      << (kinect->depth_failure().empty() ? "unknown" : kinect->depth_failure()) << '\n';
    });
}
}

namespace sfr {
KinectTuner open_kinect_tuner() {
    std::lock_guard guard(tuner_lock);
    if((tuner && !tuner->closed()) || (camera_tuner && !camera_tuner->closed())) {
        // Asked for again while open (perhaps behind another window): in
        // front again rather than nothing.
        if(tuner && !tuner->closed()) tuner->raise();
        if(camera_tuner && !camera_tuner->closed()) camera_tuner->raise();
        return KinectTuner::already_open;
    }
    bool opened=false;
    if(kinect) {
        tuner.reset();  // a closed one: its thread has ended
        tuner=KinectPreviewWindow::open_over(*kinect);
        opened=tuner!=nullptr;
        std::cerr << "NUI_KINECT_TUNER opened=" << opened << " source=kinect\n";
    } else if(camera_player) {
        camera_tuner.reset();
        camera_tuner=CameraTunerWindow::open_over(*camera_player);
        opened=camera_tuner!=nullptr;
        std::cerr << "NUI_KINECT_TUNER opened=" << opened << " source=webcam\n";
    }
    // The game waits behind the window: the controllers and the keyboard
    // read as at rest until it closes (the input update lets go).
    if(opened) {
        sfr::set_input_held(true);
        tuner_generation.fetch_add(1,std::memory_order_relaxed);
    }
    return opened ? KinectTuner::opened : KinectTuner::unavailable;
}
uint64_t kinect_tuner_generation() { return tuner_generation.load(std::memory_order_relaxed); }
// Back (Select) on a pad closes an open adjustment window, as Esc does,
// and the game's window is in front again.
void close_kinect_tuner() {
    std::lock_guard guard(tuner_lock);
    if(tuner) tuner->close();
    if(camera_tuner) camera_tuner->close();
    std::cerr << "NUI_KINECT_TUNER close=pad-back\n";
}
bool kinect_tuner_open() {
    std::lock_guard guard(tuner_lock);
    return (tuner && !tuner->closed()) || (camera_tuner && !camera_tuner->closed());
}
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
uint64_t menu_page_frame=0;  // input update count when the first player last had a menu page
uint32_t menu_page_now=0;  // the first player's menu page at menu_page_frame
std::atomic<uint32_t> title_waiting_present{0};  // present at which the title screen last waited for START
// When a ring menu last asked for its step (8246A6D0): the pause menu's wheel
// runs while the race flag is still set, and the touch stick must then turn
// it (the D-pad) instead of leaning (Issue #6).
std::atomic<int64_t> ring_asked_ms{-1000000};
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// A hand-only button (no voice word: its state at +308 lacks 0x800000, like
// the Quick Match lobby entry, type 204) takes A by steering the emulated
// right hand onto it: the hand sweeps the screen until the right cursor's
// object (+8 of the cursor at [owner+112]+116) is that button, then holds
// still so the title's own hover selection presses it.
struct HandSeek {
    uint32_t button=0, manager=0, step=0, held=0;
    // After the sweep crossed the button: points around where it did, each
    // given time for the cursor (which lags the hand) to settle.
    bool refining=false;
    float x=0, y=0;
    uint32_t candidate=0, settle=0;
} hand_seek;
constexpr uint32_t hand_seek_columns=19, hand_seek_rows=21;
// The hand-pointer dialog (823EF348) last asked at this input update, and
// its layout: while one is up, the buttons it answers to are its alone.
uint64_t dialog_frame=0;
uint32_t dialog_layout=0;
bool dialog_seen=false;
// Whether the Pause Gesture may act: in a race with no menu up. The pause
// menu's ring runs with the race flag still set (ring_asked_ms), so a ring
// asked for in the last half second, or a menu page, menu or dialog seen in
// the last half second of input updates, means a menu is open: a second
// gesture there would be START answering it.
bool pause_gesture_active(bool racing) {
    const auto recent=[](uint64_t frame) { return frame && input_frames-frame<=30; };
    return racing && !sfr::kinect_tuner_open() && now_ms()-ring_asked_ms.load(std::memory_order_relaxed)>500 &&
           !recent(menu_page_frame) && !recent(menu_manager_frame) && !(dialog_seen && recent(dialog_frame));
}
}


bool sfr::nui_body_from_sensor() { return sensor_body.load(std::memory_order_relaxed); }
uint64_t sfr::kinect_frame_generation() { return kinect_generation.load(std::memory_order_relaxed); }

// NuiInitialize(flags, ?)
SFR_INPUT_HOOK(sub_8276FD88) {
    sfr::enter_function(ctx,"sub_8276FD88",0x8276FD88);
    std::cerr << "NUI_INITIALIZE flags=0x" << std::hex << ctx.r3.u32 << " argument=0x" << ctx.r4.u32
              << std::dec << " result=0 backend=emulated-skeleton\n";
    ctx.r3.u64=0;
}

// NuiShutdown()
SFR_INPUT_HOOK(sub_8276DF30) {
    sfr::enter_function(ctx,"sub_8276DF30",0x8276DF30);
    sfr::stop_nui_skeleton_events();
    ctx.r3.u64=0;
}

// NuiSkeletonTrackingEnable(next frame event, flags): the event is signaled
// at the sensor's 30 Hz.
SFR_INPUT_HOOK(sub_82770668) {
    sfr::enter_function(ctx,"sub_82770668",0x82770668);
    std::cerr << "NUI_SKELETON_TRACKING_ENABLE event=0x" << std::hex << ctx.r3.u32 << " flags=0x" << ctx.r4.u32
              << std::dec << '\n';
    if(ctx.r3.u32) sfr::start_nui_skeleton_events(ctx.r3.u32);
    ctx.r3.u64=0;
}

// NuiSkeletonTrackingDisable()
SFR_INPUT_HOOK(sub_8276FEE0) {
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
// is thrown away. With a real Kinect these give it the
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
        return camera && std::string_view(camera)=="kinect";
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
SFR_INPUT_HOOK(sub_82768C40) {
    sfr::enter_function(ctx,"sub_82768C40",0x82768C40);
    const uint32_t type=ctx.r3.u32,resolution=ctx.r4.u32,flags=ctx.r5.u32,limit=ctx.r6.u32,event=ctx.r7.u32,
                   handle=ctx.r8.u32;
    if(kinect_images_wanted()) start_kinect();
    const bool ours=kinect && handle && ((type==0 && resolution==1 && kinect->has_depth_stream()) ||
                                         (type==1 && resolution==2 && kinect->has_colour_stream()));
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
SFR_INPUT_HOOK(sub_82767148) {
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
    // game_image: a v1's camera as it is, or a v2's made depth image
    // (a Kinect v2's), never the preview's.
    if(!kinect || !kinect->game_image(kind,stream->image) || stream->image.width!=stream->width ||
       stream->image.height!=stream->height || stream->image.bytes_per_pixel!=stream->bytes_per_pixel) {
        ctx.r3.u64=0x83010001u;  // E_NUI_FRAME_NO_DATA: the title tries again later
        return;
    }
    ++given[stream->type&1];
    if(stream->type==0) {
        const int64_t now=now_ms();
        const int64_t before=depth_given_ms.exchange(now,std::memory_order_relaxed);
        if(before>INT64_MIN/4 && now-before>500)
            std::cerr<<"NUI_KINECT_DEPTH gap ms="<<now-before<<'\n';
        depth_given_number.store(stream->image.number,std::memory_order_relaxed);
        depth_given_count.fetch_add(1,std::memory_order_release);
        // SFR_KINECT_RECORD_DEPTH (kinect_record_depth.h): every image the
        // game is handed, as it is handed (a v1's own, or the one a v2 made).
        if(sfr::KinectDepthRecordFile* file=sfr::kinect_record_depth_file("")) {
            sfr::KinectDepthRecordFrame record;
            record.steady_us=stream->image.steady_us;
            record.handed_us=sfr::kinect_record_now_us();
            record.sdk_time=stream->image.sdk_time;
            record.number=stream->image.number;
            record.sdk_frame=stream->image.sdk_frame;
            record.source=std::string_view(kinect->model())=="Kinect v2"?sfr::KinectDepthSource::v2:sfr::KinectDepthSource::v1;
            record.origin=stream->image.view_origin;
            record.z_scale=stream->image.view_z_scale;
            sfr::kinect_depth_frame_pixels(record,stream->image.pixels.data(),stream->width,stream->height);
            file->write(record);
        }
    }
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
SFR_CONCURRENT_HOOK(sub_824F3CF0) {
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
SFR_INPUT_HOOK(sub_82767458) {
    sfr::enter_function(ctx,"sub_82767458",0x82767458);
    if(!image_stream(ctx.r3.u32)) { __imp__sub_82767458(ctx,base); return; }
    ctx.r3.u64=0;
}

// One of a real Kinect's skeletons in its own slot of the frame
// (NUI_SKELETON_DATA: state, tracking id, enrollment, user index, position,
// 20 joints, their states).
// The user index (+12) is the signed-in user the skeleton plays as, 0..3:
// the menus' player slot takes an identified player only with one below 4
// (82491458), and the save is that user's. It is not the skeleton's slot.
void write_kinect_body(sfr::GuestMemory& memory, uint32_t frame, const sfr::KinectBody& body, uint32_t enrollment,
                       uint32_t user) {
    const uint64_t data=uint64_t(frame)+sfr::nui_skeleton_data_offset+uint64_t(body.sensor_index)*sfr::nui_skeleton_data_size;
    const auto vector=[&](uint64_t address,const std::array<float,3>& v) {
        for(uint32_t i=0;i<3;++i) memory.store<uint32_t>(address+4*i,std::bit_cast<uint32_t>(v[i]));
        memory.store<uint32_t>(address+12,std::bit_cast<uint32_t>(1.0f));
    };
    memory.store<uint32_t>(data,sfr::nui_tracked);
    memory.store<uint32_t>(data+4,body.tracking_id);
    memory.store<uint32_t>(data+8,enrollment);
    memory.store<uint32_t>(data+12,user);
    vector(data+16,body.position);
    for(uint32_t j=0;j<sfr::nui_joint_count;++j) {
        vector(data+32+j*16,body.joints[j]);
        memory.store<uint32_t>(data+352+j*4,std::min(body.joint_states[j],sfr::nui_tracked));
    }
}

// NuiSkeletonGetNextFrame(timeout ms, frame): one emulated player.
SFR_INPUT_HOOK(sub_827707B0) {
    sfr::enter_function(ctx,"sub_827707B0",0x827707B0);
    sfr::poll_window_title();  // the frame rate in the window title
    {
        // Back on any pad closes the adjustment window (the game reads the
        // pads as at rest meanwhile); a press already down when it opened
        // does not count.
        static bool was_open=false, was_back=false;
        const bool open=sfr::kinect_tuner_open();
        const bool back=open && (sfr::nui_pad_buttons_through_hold() & sfr::gamepad_button::back);
        if(open && was_open && back && !was_back) sfr::close_kinect_tuner();
        was_open=open;
        was_back=back;
    }
    const uint32_t frame=ctx.r4.u32;
    if(!frame) { ctx.r3.u64=0x80004003u; return; }  // E_POINTER, as the original
    // [83E52F8C] is the race flag: the hands leave the menu cursor pose.
    const bool racing=sfr::active_memory->load<uint32_t>(0x83E52F8C) != 0;
    sfr::set_touch_racing(racing && now_ms()-ring_asked_ms.load(std::memory_order_relaxed)>150);
    {
        // The title screen waits for START (its state 7..10, updated within
        // the last few frames) and shows no dialog: a touch there is START.
        const uint32_t seen=title_waiting_present.load(std::memory_order_relaxed), now=sfr::present_count.load();
        const bool title=!racing && seen && now-seen<=10 && !(dialog_seen && input_frames-dialog_frame<=30);
        static bool was_title=false;
        if(title!=was_title) std::cerr << "NUI_TITLE_WAITING " << (title?"on":"off") << " present=" << now << '\n';
        was_title=title;
        sfr::set_touch_title(title);
        // A story scene (Issue #274), as START's skip below sees one: no menu
        // page, dialog or ring for half a second of input updates, not racing,
        // not the title. The touch controls hide there.
        const bool story=!racing && !title && input_frames>30 && input_frames-menu_page_frame>30 &&
                         !(dialog_seen && input_frames-dialog_frame<=30) &&
                         now_ms()-ring_asked_ms.load(std::memory_order_relaxed)>500 && !sfr::kinect_tuner_open();
        static bool was_story=false;
        if(story!=was_story)
            std::cerr << "NUI_TOUCH_STORY " << (story?"on":"off") << " present=" << now
                      << " page_age=" << (input_frames-menu_page_frame)
                      << " manager_age=" << (input_frames-menu_manager_frame) << '\n';
        was_story=story;
        sfr::set_touch_story(story);
    }
    const char* const choice=std::getenv("SFR_CAMERA");
    if(choice && std::string_view(choice)=="kinect") {
        // Every Kinect caller waits for initialization, including one that
        // arrives while an image-stream request is opening the backend.
        start_kinect();
    } else if(!camera_started) {
        camera_started=true;
        camera_debug=sfr::CameraDebugWindow::start();
        camera_player=sfr::CameraPlayer::start();
    }
    if(kinect) {
        static const bool motion_trace=sfr::motion_trace_enabled();
        const auto work_began=std::chrono::steady_clock::now();
        // How far the menu steadying moved each body's hands this frame.
        static std::unordered_map<uint32_t,std::array<float,2>> steady_mm;
        // Both players are whoever the sensor sees; nobody in front of it is
        // an empty frame, which the title answers the way it did on the
        // console (asking the player to step in). The pads keep their voice
        // commands and menu buttons (the input update below).
        auto& memory=*sfr::active_memory;
        // While the adjustment window is open the game is given one frame
        // with every arm hanging down, then nothing new: no cursor, no
        // gesture, nobody leaving. The window sees the sensor throughout.
        const bool tuning=sfr::kinect_tuner_open();
        static bool arms_given=false;
        static sfr::KinectFrame tuner_frame;
        if(!tuning) {
            if(arms_given) std::cerr << "NUI_KINECT_TUNER input=released\n";
            arms_given=false;
            sfr::set_input_held(false);
        }
        // Turned once per new frame: the last one is kept as it was turned.
        bool received=false, expired=false;
        if(tuning && arms_given) {
            if(kinect->next(tuner_frame)) {
                std::lock_guard guard(tuner_lock);
                if(tuner && !tuner->closed()) tuner->show_frame(tuner_frame);
            }
        } else {
            received=kinect->next(kinect_frame);
            if(received) {
                sfr::note_kinect_frame();  // for the window's title
                // The menu cursor steadied (kinect_stability.h) before
                // anything reads the frame.
                static sfr::KinectBodySteadying steadying=sfr::KinectBodySteadying::from_environment();
                std::vector<std::pair<uint32_t,std::array<std::array<float,3>,2>>> raw;
                if(motion_trace)
                    for(const sfr::KinectBody& body:kinect_frame.bodies)
                        raw.push_back({body.tracking_id,{body.joints[sfr::nui_joint::hand_left],
                                                         body.joints[sfr::nui_joint::hand_right]}});
                steadying.steady(kinect_frame.bodies,!racing);
                steady_mm.clear();
                for(const auto& [id,hands]:raw)
                    for(const sfr::KinectBody& body:kinect_frame.bodies) {
                        if(body.tracking_id!=id) continue;
                        auto& shift=steady_mm[id];
                        for(int side=0;side<2;++side) {
                            const auto& a=hands[side];
                            const auto& b=body.joints[side?sfr::nui_joint::hand_right:sfr::nui_joint::hand_left];
                            shift[side]=1000.0f*std::sqrt((a[0]-b[0])*(a[0]-b[0])+(a[1]-b[1])*(a[1]-b[1])+
                                                          (a[2]-b[2])*(a[2]-b[2]));
                        }
                    }
            }
            expired=sfr::kinect_expire_frame(kinect_frame,std::chrono::steady_clock::now());
            // The adjustment window sees the frames the game reads, not its own.
            if(received) {
                std::lock_guard guard(tuner_lock);
                if(tuner && !tuner->closed()) tuner->show_frame(kinect_frame);
            }
            if(tuning && received) {
                for(sfr::KinectBody& body:kinect_frame.bodies) sfr::kinect_arms_down(body);
                arms_given=true;
                std::cerr << "NUI_KINECT_TUNER input=held bodies=" << kinect_frame.bodies.size() << '\n';
            }
        }
        if(received || expired) {
            // The title stores two fully tracked body indices, even though
            // the sensor frame has six slots. Keep the same two real bodies
            // until they leave, preserving their source slot numbers.
            kinect_players.retain(kinect_frame);
            ++frame_number;
            const float gravity_y=kinect_frame.gravity[1];
            const uint64_t generation=kinect_generation.fetch_add(1,std::memory_order_relaxed)+1;
            // Every three seconds, what reaches the title: the tilt taken
            // out and the first body's centre line and hands, with the hands'
            // tracking states (2 tracked, 1 inferred, 0 not).
            if(generation%90==1) {
                std::ostringstream line;
                line<<"NATIVE_KINECT_BODY frame="<<generation<<" gravity_y="<<gravity_y
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
        // A cached body retains the source capture time and frame number.
        // Expiry publishes one empty tracking update, then stays empty until
        // the acquisition worker supplies a new frame.
        const auto captured_ms=std::chrono::duration_cast<std::chrono::milliseconds>(kinect_frame.captured_at-started).count();
        sfr::NuiSkeletonEmulation::write_header(memory,frame,frame_number,uint64_t(std::max<int64_t>(0,captured_ms)));
        sfr::NuiSkeletonEmulation::write_floor(memory,frame,kinect_frame.floor_plane,kinect_frame.gravity);
        {
            std::lock_guard guard(kinect_identity_lock);
            // Ids the sensor no longer tracks are gone: whoever steps in next
            // is a new skeleton.
            sfr::kinect_prune_identities(kinect_frame,kinect_identities);
            // The profile is user 0; everyone else a user after it, in the
            // sensor's order, as the pads' players were 1, 2 and 3.
            uint32_t next_user=1;
            for(const sfr::KinectBody& body:kinect_frame.bodies) {
                if(body.sensor_index>=6) continue;
                const auto known=kinect_identities.find(body.tracking_id);
                const uint32_t enrollment=known==kinect_identities.end()?sfr::NuiSkeletonEmulation::unidentified:known->second;
                const uint32_t user=enrollment==0?0u:std::min(next_user++,3u);
                write_kinect_body(memory,frame,body,enrollment,user);
            }
        }
        static std::vector<uint32_t> seen;
        for(const sfr::KinectBody& body:kinect_frame.bodies)
            if(std::find(seen.begin(),seen.end(),body.tracking_id)==seen.end())
                std::cerr << "NATIVE_KINECT_ENTER tracking_id=" << body.tracking_id << " slot=" << body.sensor_index << '\n';
        seen.clear();
        for(const sfr::KinectBody& body:kinect_frame.bodies) seen.push_back(body.tracking_id);
        {
            // Anyone the sensor tracks may pause, as on the console: the
            // console's pose, or a Kinect v2's Stop Sign.
            const bool pause_active=pause_gesture_active(racing);
            watch_body_pause_gestures(kinect_frame,pause_active);
            // An open hand raised where it can pause: which way the v2 says
            // its palm faces, a line a second, to check the Stop Sign on a
            // real sensor (in the menus a raised hand is the cursor).
            static auto told=std::chrono::steady_clock::time_point{};
            const auto now=std::chrono::steady_clock::now();
            if(pause_active && now-told>=std::chrono::seconds(1) && !kinect_frame.bodies.empty()) {
                const sfr::KinectBody& body=kinect_frame.bodies.front();
                for(int side=0;side<2;++side) {
                    const auto& hand=body.joints[side==0?sfr::nui_joint::hand_left:sfr::nui_joint::hand_right];
                    if(body.hand_states[side]!=sfr::PauseGesture::open_hand ||
                       hand[1]<body.joints[sfr::nui_joint::shoulder_center][1]-0.05f) continue;
                    told=now;
                    std::cerr << "NUI_STOP_SIGN hand=" << (side?"right":"left") << " palm_z="
                              << sfr::PauseGesture::palm_facing(body.joints,side,body.hand_tips,body.thumbs)
                              << " stop_sign=" << sfr::PauseGesture::stop_sign(body.joints,body.hand_states,body.hand_tips,body.thumbs) << '\n';
                }
            }
        }
        if((kinect_frame.bodies.size()>=2)!=second_present) {
            second_present=kinect_frame.bodies.size()>=2;
            std::cerr << "NUI_SECOND_PLAYER present=" << second_present << " source=kinect\n";
        }
        if(motion_trace) {
            // This hook's own share of a frame with a Kinect (taking the
            // sensor's frame, steadying, levelling, writing the skeletons,
            // the pause gestures), every 300 calls: it should be
            // microseconds; a slow race would show it here first.
            static sfr::TraceCost cost;
            cost.add(std::chrono::steady_clock::now()-work_began);
            if(cost.calls()>=300) {
                std::cerr << "KINECT_COST part=skeleton calls=" << cost.calls() << " mean_us=" << cost.mean_us()
                          << " max_us=" << cost.max_us() << '\n';
                cost.clear();
            }
        }
        ctx.r3.u64=0;
        return;
    }
    const bool debug_active=camera_debug && !camera_debug->closed();
    sfr::CameraTrackingStatus camera_status;
    if(camera_player) camera_player->set_in_menu(!racing);
    // While the adjustment window is open, as with the Kinect: the arms put
    // down once, then the pose kept (the camera's freshness still read, so
    // the camera stays the player's source).
    static bool camera_arms_given=false;
    const bool camera_tuning=camera_player && sfr::kinect_tuner_open();
    if(camera_tuning) {
        sfr::SkeletonJoints ignored{};
        camera_player->joints(ignored,&camera_status);
        if(!camera_arms_given && camera_has_joints) {
            sfr::arms_down(camera_joints);
            camera_pose_counter.fetch_add(1,std::memory_order_relaxed);
            camera_arms_given=true;
            std::cerr << "NUI_KINECT_TUNER input=held source=webcam\n";
        }
    } else {
        if(camera_arms_given) std::cerr << "NUI_KINECT_TUNER input=released source=webcam\n";
        camera_arms_given=false;
        if(camera_player) sfr::set_input_held(false);
        if(camera_player && camera_player->joints(camera_joints,&camera_status)) {
            camera_has_joints=true;
            camera_pose_counter.fetch_add(1,std::memory_order_relaxed);
        }
    }
    const auto first=sfr::nui_gamepad();
    const bool use_camera=camera_selection.update(first,camera_has_joints?camera_status.pose_age_ms:-1,
                                                  sfr::CameraInputSelection::Clock::now());
    const bool was_camera=sfr::camera_motion_active();
    camera_input_active.store(use_camera,std::memory_order_relaxed);
    if(use_camera) watch_pause_gesture(sfr::PauseGesture::in_pose(camera_joints),pause_gesture_active(racing));
    else {
        pause_gesture.forget();
        sfr::publish_pause_gauge(0.0f);
    }
    if(use_camera!=was_camera)
        std::cerr<<"NUI_INPUT_SOURCE player=0 source="<<(use_camera?"camera":"controller")
                 <<" pose_age_ms="<<camera_status.pose_age_ms<<'\n';
    // Camera supplements P1. Each configured controller keeps its player,
    // including while the camera opens, loses tracking, or is overridden.
    // Relay Race (team mode 3 at 83E515E7) is one person in front of the
    // sensor at a time, so a second controller is not a second person there:
    // it takes its turn through relay_swap below.
    const bool relay=sfr::active_memory->load<uint8_t>(0x83E515E7)==3;
    const auto second=relay ? std::optional<sfr::GamepadState>{} : sfr::nui_second_gamepad(1u, racing);
    // START in a story scene (no menu page, no dialog, not racing): the World
    // Grand Prix scenes end through a SKIP button that only a hand resting on
    // it presses, and a parked hand never reaches it. The hand moves to the
    // centre where that button is, as a nudge of the right stick does.
    {
        static bool start_held=false;
        const bool start=(first.buttons & sfr::gamepad_button::start)!=0;
        if(start && !start_held && !racing && !use_camera &&
           input_frames-menu_page_frame>30 && !(dialog_seen && input_frames-dialog_frame<=30)) {
            auto& own=player_routing.reversed()?second_skeleton:skeleton;
            if(own.centre_cursor()) std::cerr << "NUI_SCENE_SKIP_CURSOR frame=" << input_frames << '\n';
        }
        start_held=start;
    }
    const bool was_reversed=player_routing.reversed();
    player_routing.update(*sfr::active_memory,skeleton,second_skeleton,
                          first,second,racing,use_camera,was_camera && !use_camera);
    if(hand_seek.button && !racing) {
        // A list entry (kind 2) is pressed by pushing the hand towards the
        // sensor. Each candidate place on the page (screen pixels of 1280x720;
        // measured on this page with the hand raised: px = 312 + 1762 x,
        // py = 685 - 1376 y) gets a glide, a 0.35 m push and a release.
        auto& seek_memory=*sfr::active_memory;
        const auto word=[&](uint64_t at) { return seek_memory.readable(at,4) ? seek_memory.load<uint32_t>(at) : 0u; };
        static constexpr float targets[][2]={{1008,608},{975,600},{1040,615},{1008,580},{1008,635}};
        constexpr uint32_t per_target=105, raise=45;
        // The title tracks a menu cursor only once the hand has been raised
        // above the shoulder (nui_skeleton.cpp); after a page change it has
        // usually dropped, so raise it first and give the cursor time to wake.
        if(hand_seek.held<raise) {
            skeleton.place_right(0.175f,0.45f);
            if(++hand_seek.held==raise) {
                const uint32_t owner=word(uint64_t(hand_seek.manager)+96);
                const uint32_t hands=owner ? word(uint64_t(owner)+112) : 0;
                std::cerr << "NUI_HAND_SEEK raised cursor_state=" << (hands ? word(uint64_t(hands)+116+100) : ~0u) << '\n';
            }
        }
        const uint32_t target=hand_seek.step/per_target, at=hand_seek.step%per_target;
        if(hand_seek.held<raise) {
        } else if(target>=sizeof(targets)/sizeof(targets[0])) {
            std::cerr << "NUI_HAND_SEEK done button=0x" << std::hex << hand_seek.button << std::dec << '\n';
            hand_seek={};
        } else {
            const float x=(targets[target][0]-312.0f)/1762.0f, y=(685.0f-targets[target][1])/1376.0f;
            const auto hand=skeleton.hand(true);
            if(at<30) {
                const float t=float(at+1)/30.0f;
                skeleton.place_right(hand[0]+(x-hand[0])*t,hand[1]+(y-hand[1])*t);
            } else if(at<90) {
                if(at==30) {
                    const uint32_t owner=word(uint64_t(hand_seek.manager)+96);
                    const uint32_t hands=owner ? word(uint64_t(owner)+112) : 0;
                    std::cerr << "NUI_HAND_SEEK push px=" << targets[target][0] << ',' << targets[target][1]
                              << " cursor_state=" << (hands ? word(uint64_t(hands)+116+100) : ~0u) << '\n';
                }
                skeleton.place_right(x,y,2.25f-0.35f*float(at-30)/60.0f);
            } else {
                skeleton.place_right(x,y);
            }
            ++hand_seek.step;
        }
    }
    // A finger on the screen or the mouse over the game points the menu
    // cursor (Issue #217): the hand goes where the cursor lands on that spot
    // and stays there, so the item under it fills and is chosen. The pad's
    // right stick moves it on from there.
    {
        static uint32_t applied_serial=0, last_page=0;
        static bool last_dialog=false;
        const sfr::PointerTarget pointer=sfr::pointer_target();
        auto& pointed=player_routing.reversed()?second_skeleton:skeleton;
        // A choice made by pointing changes the page or opens or closes a
        // dialog. The hand then leaves the spot (to the corner where the
        // cursor waits clear of buttons) unless a finger still holds it:
        // otherwise it would choose whatever the next page has there.
        const uint32_t page_now=input_frames-menu_page_frame<=2 ? menu_page_now : 0;
        const bool dialog_now=dialog_seen && input_frames-dialog_frame<=2;
        if((page_now!=last_page || dialog_now!=last_dialog) && pointed.pointed() && !pointer.held) {
            pointed.park_pointed();
            std::cerr << "NUI_POINTER parked page=0x" << std::hex << page_now << std::dec << " dialog=" << dialog_now
                      << " frame=" << input_frames << '\n';
        }
        last_page=page_now;
        last_dialog=dialog_now;
        if(pointer.serial!=applied_serial) {
            applied_serial=pointer.serial;
            if(!racing && !use_camera && !hand_seek.button) {
                // A page whose cursor is hidden (state 2) shows it only after a
                // hand is raised above the shoulder: a hand already low would
                // go to the spot without ever raising it.
                uint32_t cursor_state=~0u;
                auto& memory=*sfr::active_memory;
                if(menu_manager && input_frames-menu_manager_frame<=2) {
                    const uint32_t owner=memory.load<uint32_t>(menu_manager+96);
                    const uint32_t hands=owner && memory.readable(uint64_t(owner)+112,4) ? memory.load<uint32_t>(uint64_t(owner)+112) : 0;
                    if(hands && memory.readable(uint64_t(hands)+116+100,4)) cursor_state=memory.load<uint32_t>(uint64_t(hands)+116+100);
                }
                pointed.point_right(pointer.x*1280.0f,pointer.y*720.0f,cursor_state==2);
                static uint32_t told=0;
                if(told++<200)
                    std::cerr << "NUI_POINTER x=" << pointer.x*1280.0f << " y=" << pointer.y*720.0f
                              << " frame=" << input_frames << '\n';
            }
        }
    }
    if(was_reversed!=player_routing.reversed())
        std::cerr << "NUI_PLAYER_ROUTING first_tracking_id=" << (player_routing.reversed()?2:1)
                  << " second_tracking_id=" << (player_routing.reversed()?1:2) << '\n';
    // The race hooks need the same pad, and this is where it is decided
    // which one the second player is holding.
    sfr::publish_second_player_pad(second);
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started);
    auto& memory=*sfr::active_memory;
    sfr::NuiSkeletonEmulation::write_header(memory,frame,++frame_number,uint64_t(elapsed.count()));
    auto& relay_body=player_routing.reversed()?second_skeleton:skeleton;
    if(const uint32_t out=relay_out.load(std::memory_order_relaxed)) {
        // A relay swap: nobody in view for a moment, then the next racer, a
        // new person (tracking id 3, 4, ...) the title identifies again.
        if(out==1) {
            const uint32_t id=relay_tracking_id.load(std::memory_order_relaxed);
            relay_tracking_id.store(id<3?3:id+1,std::memory_order_relaxed);
            relay_body.forget();
            sfr::set_relay_pad(relay_next_pad.load(std::memory_order_relaxed));
            std::cerr << "NUI_RELAY_SWAP in tracking_id=" << relay_tracking_id.load(std::memory_order_relaxed)
                      << " controller=" << sfr::relay_pad() << '\n';
        }
        relay_out.store(out-1,std::memory_order_relaxed);
    } else if(relay && relay_tracking_id.load(std::memory_order_relaxed)) {
        relay_body.write_slot(memory,frame,0,relay_tracking_id.load(std::memory_order_relaxed));
    } else
    // Submit exactly one source for logical P1; never change slot identities.
    player_routing.write_slots(memory,frame,skeleton,second_skeleton,second.has_value(),
                               use_camera?&camera_joints:nullptr);
    // Numeric-only race diagnostics: the exact submitted P1 joints, never
    // camera pixels. Pair with SFR_RACE_BODY_DUMP to inspect guest processing.
    static const bool race_trace=sfr::motion_trace_enabled();
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

PPC_FUNC_IMPL(__imp__sub_82764728);

// The Kinect library's identity of an enrollment (enrollment, out): out[0]
// the user index that enrollment signed in as, out[1] nonzero when it has
// one; an enrollment of 8 or more is E_INVALIDARG. With a second player the
// title loads each player's profile (82491458, a request the title screen
// and the menus wait for) and asks this with the player's enrollment until
// the player is a signed-in user. A guest's enrollment (0xFFFFFFFE) is
// refused, and the title retries until a 10-second timer gives up -- the
// pause after the second player's figure walks across the title. An
// enrollment without a user gives up the same request at once, the same
// way, so that is the answer a guest gets from this one caller.
SFR_INPUT_HOOK(sub_82764728) {
    sfr::enter_function(ctx,"sub_82764728",0x82764728);
    const uint32_t enrollment=ctx.r3.u32, out=ctx.r4.u32, caller=uint32_t(ctx.lr);
    if(enrollment>=8 && out && caller>=0x82491458 && caller<0x82491C78) {
        auto& memory=*sfr::active_memory;
        memory.store<uint32_t>(out,254);
        memory.store<uint32_t>(out+4,0);
        ctx.r3.u64=0;
        static uint32_t told=0;
        if(told++<4) std::cerr << "NUI_GUEST_PROFILE enrollment=0x" << std::hex << enrollment << std::dec << " answer=none\n";
        return;
    }
    __imp__sub_82764728(ctx,base);
}

// NuiIdentityIdentify(tracking id, flags, callback, context): the title
// identifies each new skeleton before it joins as a player (82437E48). The
// emulated player is the local profile (enrollment 0) when one is signed in,
// so the title plays as that profile and keeps its records; otherwise an
// unenrolled guest. The completion message (id 1, result S_OK, enrollment)
// is delivered to the callback at once and the call reports the pending
// asynchronous operation it started.
SFR_INPUT_HOOK(sub_82764620) {
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
        // A relay's next racer (3, 4, ...) is the one person in view.
        (tracking_id==2 || (tracking_id>=3 && player_routing.reversed()) ? second_skeleton : skeleton)
            .identify(profile?0u:sfr::NuiSkeletonEmulation::guest);
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

PPC_FUNC_IMPL(__imp__sub_8244F880);
// The title screen's update (this+4 its state): 7..9 start the title movie,
// 10 waits for START (said "start", 82494658 below), 12.. leave for the menus.
// Its present is noted while it waits, for the touch controls (Issue #217).
SFR_CONCURRENT_HOOK(sub_8244F880) {
    sfr::enter_function(ctx,"sub_8244F880",0x8244F880);
    const uint32_t title=ctx.r3.u32;
    __imp__sub_8244F880(ctx,base);
    const uint32_t state=sfr::active_memory->load<uint32_t>(uint64_t(title)+4);
    if(state>=7 && state<=10)
        title_waiting_present.store(std::max(1u,sfr::present_count.load()),std::memory_order_relaxed);
}

PPC_FUNC_IMPL(__imp__sub_8244C7A8);

// The startup logos (Issue #217): the ESRB notice (North American consoles
// only), SEGA and Sonic Team are one object the title draws itself
// (8244CA28), not movies. Its update, this one, keeps the step at +20: 2
// picks the first logo, 3..5 show ESRB, SEGA and Sonic Team, each until its
// fade ends (8244C710), then 0, and 1 removes the object; the title screen
// follows as soon as its own loading allows. SFR_SKIP_LOGOS=1 (the launcher's
// Skip logos) goes straight to the end; without it A, B, START or BACK ends
// the logos, as they end a movie; so does a touch, which is START there.
SFR_CONCURRENT_HOOK(sub_8244C7A8) {
    sfr::enter_function(ctx,"sub_8244C7A8",0x8244C7A8);
    static const bool skip_all=[]{ const char* t=std::getenv("SFR_SKIP_LOGOS"); return t && *t && *t!='0'; }();
    constexpr uint16_t skip_buttons=sfr::gamepad_button::a|sfr::gamepad_button::b|sfr::gamepad_button::start|
                                    sfr::gamepad_button::back;
    const uint32_t logos=ctx.r3.u32;
    auto& memory=*sfr::active_memory;
    const uint32_t step=memory.load<uint32_t>(uint64_t(logos)+20);
    if(step>=2 && step<=5 && (skip_all || (sfr::nui_gamepad().buttons & skip_buttons))) {
        memory.store<uint32_t>(uint64_t(logos)+20,0);
        std::cerr << "NUI_LOGO_SKIP step=" << step << " present=" << sfr::present_count.load()
                  << (skip_all ? " setting" : " button") << '\n';
    }
    __imp__sub_8244C7A8(ctx,base);
    // A touch on the logos is START too (touch_controls.h).
    if(memory.load<uint32_t>(uint64_t(logos)+20)>=2)
        title_waiting_present.store(std::max(1u,sfr::present_count.load()),std::memory_order_relaxed);
}

PPC_FUNC_IMPL(__imp__sub_824920F8);
namespace {
// SFR_NUI_ANALYSIS_TRACE=1 (nui_analysis_trace.h): the menu hand cursor as
// 82492F30 worked it out on this input update, for each player with a body,
// at most ten lines a second. Per hand (packet P = input + 32 + 2544 player
// + 16 + 400 hand): its flags (P+0: 0x8 raised, 0x10 lowered), theta (P+60,
// the angle in degrees between shoulder - hand and the x axis, in 3D), r
// (P+148, (head - hand).y / (head - spine).y), the target (P+96/+100,
// pixels), the cursor (P+80/+84, pixels, the target smoothed by 0.065 a
// frame; what the menus read) and what the study's formula gives from theta
// and r (x = 1280 (135 - theta) / 90, y = 720 (r - 0.23)). Then the joints
// the input saw (its copy at +1264 of the player's block: the record's
// joints after the title's smoothing and its mirror, (-x, y, -z)).
void trace_cursor(uint32_t input) {
    if(!sfr::nui_analysis_trace() || !input) return;
    auto& memory=*sfr::active_memory;
    const auto word=[&](uint64_t at) {return memory.readable(at,4)?memory.load<uint32_t>(at):0u;};
    const auto number=[&](uint64_t at) {return std::bit_cast<float>(word(at));};
    uint32_t players=0;
    for(uint32_t player=0;player<2;++player) players+=word(uint64_t(input)+32+2544*player+12)!=0;
    static sfr::NuiTraceLimiter limiter("CURSOR",10.0,36000);
    if(!players || !limiter.take(players)) return;
    const bool racing=word(0x83E52F8C)!=0;
    for(uint32_t player=0;player<2;++player) {
        const uint64_t block=uint64_t(input)+32+2544*player;
        const uint32_t body=word(block+12);
        if(!body) continue;
        const uint32_t data=word(uint64_t(body)+768);
        std::ostringstream line;
        line<<"NUI_TRACE_CURSOR ms="<<now_ms()<<" player="<<player<<" frame="<<input_frames<<" racing="<<racing
            <<" tracking_id="<<(data?word(uint64_t(data)+4):0)<<" body_slot="<<int32_t(word(uint64_t(body)+784));
        for(uint32_t hand=0;hand<2;++hand) {
            const uint64_t packet=block+16+400*hand;
            const float theta=number(packet+60),r=number(packet+148);
            line<<(hand?" right[":" left[")<<"flags=0x"<<std::hex<<word(packet)<<std::dec
                <<" theta="<<theta<<" r="<<r
                <<" target="<<number(packet+96)<<','<<number(packet+100)
                <<" cursor="<<number(packet+80)<<','<<number(packet+84)
                <<" formula="<<1280.0f*(135.0f-theta)/90.0f<<','<<720.0f*(r-0.23f)<<']';
        }
        for(const auto& [name,joint] : {std::pair{"hand_l",sfr::nui_joint::hand_left},std::pair{"hand_r",sfr::nui_joint::hand_right},
                                       std::pair{"shoulder_l",sfr::nui_joint::shoulder_left},
                                       std::pair{"shoulder_r",sfr::nui_joint::shoulder_right},
                                       std::pair{"head",sfr::nui_joint::head},std::pair{"spine",sfr::nui_joint::spine}}) {
            const uint64_t at=block+1264+16*joint;
            line<<' '<<name<<'='<<number(at)<<','<<number(at+4)<<','<<number(at+8);
        }
        std::cerr<<line.str()<<'\n';
    }
}
}
namespace {
// SFR_KINECT_RECORD with a real Kinect (kinect_nui_record.h): a row of
// <prefix>-nui.csv per player with a body record (or with anything the race
// gathered for it) on each input update: the cursor as 82492F30 left it,
// the record's joints (after the title's smoothing, mirrored), and what the
// race detectors and the race preparation's measurements gave since the
// player's last row. Queued: the game never waits for the disk.
void record_nui_rows(uint32_t input) {
    if(!input || !sfr::nui_body_from_sensor()) return;
    sfr::KinectRecordFile* file=sfr::kinect_record_nui();
    if(!file) return;
    auto& memory=*sfr::active_memory;
    const auto word=[&](uint64_t at) {return memory.readable(at,4)?memory.load<uint32_t>(at):0u;};
    const auto number=[&](uint64_t at) {return std::bit_cast<float>(word(at));};
    const bool racing=word(0x83E52F8C)!=0;
    const int64_t now=sfr::kinect_record_now_us();
    std::string text;
    for(uint32_t player=0;player<2;++player) {
        sfr::KinectNuiRecord row;
        sfr::kinect_nui_recorder().take(player,row);
        const uint64_t block=uint64_t(input)+32+2544*player;
        const uint32_t body=word(block+12);
        const bool gathered=row.has_lean || !row.calibration_step.empty() ||
            std::any_of(row.detectors.begin(),row.detectors.end(),[](const auto& answer){return answer.result>=0;});
        if(!body && !gathered) continue;
        row.steady_us=now;
        row.update=input_frames;
        row.player=player;
        row.racing=racing;
        if(body) {
            const uint32_t data=word(uint64_t(body)+768);
            row.tracking_id=data?word(uint64_t(data)+4):0;
            row.body_slot=int32_t(word(uint64_t(body)+784));
            row.has_cursor=true;
            for(uint32_t hand=0;hand<2;++hand) {
                const uint64_t packet=block+16+400*hand;
                sfr::KinectNuiHand& out=row.hands[hand];
                out.flags=word(packet);
                out.theta=number(packet+60);
                out.r=number(packet+148);
                out.target_x=number(packet+96);
                out.target_y=number(packet+100);
                out.cursor_x=number(packet+80);
                out.cursor_y=number(packet+84);
            }
            row.has_joints=true;
            for(uint32_t i=0;i<sfr::kinect_nui_joint_count;++i)
                for(uint32_t axis=0;axis<3;++axis)
                    row.joints[i][axis]=number(uint64_t(body)+16*sfr::kinect_nui_joints[i]+4*axis);
        }
        sfr::kinect_nui_record_row(row,text);
    }
    if(!text.empty()) file->write(std::move(text));  // queued: never waits for the disk
}
}
// The title's START screen does not use every later menu-manager path.
// Observe the completed common input update as well as the menu cursor below.
SFR_MENU_HOOK(sub_824920F8) {
    sfr::enter_function(ctx,"sub_824920F8",0x824920F8);
    const uint32_t input=ctx.r3.u32;
    __imp__sub_824920F8(ctx,base);
    try {
        trace_cursor(input);
        record_nui_rows(input);
    } catch(const std::exception& error) {
        std::cerr<<"NUI_TRACE_CURSOR unreadable="<<error.what()<<'\n';
    }
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
SFR_MENU_HOOK(sub_82494658) {
    sfr::enter_function(ctx,"sub_82494658",0x82494658);
    const uint32_t input=ctx.r3.u32;
    auto& memory=*sfr::active_memory;
    __imp__sub_82494658(ctx,base);
    const bool racing=memory.load<uint32_t>(0x83E52F8C)!=0;
    pressed=edges.update(sfr::nui_gamepad().buttons);
    const auto second_pad=sfr::second_player_pad();
    second_pressed=second_edges.update(second_pad ? second_pad->buttons : 0);
    ++input_frames;
    // Relay Race: "Swap out with the next player" (the menu manager's +0x68
    // bit 0x8) waits for the racer to leave the sensor and somebody new to
    // step in. A or BACK (the No Kinect Patch's swap, Issue #109) on any
    // controller is that swap, and the controller that pressed it drives the
    // next racer.
    {
        static sfr::NuiPadEdges relay_edges[4];
        uint16_t relay_pressed[4]{};
        for(uint32_t user=0; user<4; ++user) {
            const auto pad=sfr::nui_pad(user);
            relay_pressed[user]=relay_edges[user].update(pad ? pad->buttons : 0);
        }
        const bool relay=memory.load<uint8_t>(0x83E515E7)==3;
        // In a race the racer stops at a gate at the end of their leg: the
        // racer's state ([83E53160]+0x1C4) is 5 while racing, 3 at the gate,
        // 4 while the next one comes in. One swap per arrival at the gate, as
        // the state reads 3 again for a moment after the swap.
        const uint32_t racer=memory.load<uint32_t>(0x83E53160);
        const uint32_t racer_state=racer && memory.readable(uint64_t(racer)+0x1C4,4) ?
                                   memory.load<uint32_t>(uint64_t(racer)+0x1C4) : ~0u;
        static uint32_t last_racer_state=~0u;
        static bool gate_armed=false;
        if(racer_state!=last_racer_state) {
            if(relay && racing && racer_state==3 && last_racer_state==5) {
                gate_armed=true;
                std::cerr << "NUI_RELAY_GATE waiting\n";
            }
            last_racer_state=racer_state;
        }
        if(racer_state!=3) gate_armed=false;
        const bool wants_swap=relay && ((racing && gate_armed) ||
            (menu_manager && input_frames-menu_manager_frame<=2 && (memory.load<uint32_t>(menu_manager+0x68)&8)));
        if(wants_swap && !relay_out.load(std::memory_order_relaxed)) {
            constexpr uint16_t swap_buttons=sfr::gamepad_button::a | sfr::gamepad_button::back;
            for(uint32_t user=0; user<4; ++user) {
                if(!(relay_pressed[user] & swap_buttons)) continue;
                relay_next_pad.store(user,std::memory_order_relaxed);
                relay_out.store(45,std::memory_order_relaxed);
                gate_armed=false;
                pressed&=uint16_t(~swap_buttons);  // the swap, not an "ok"
                std::cerr << "NUI_RELAY_SWAP out controller=" << user << '\n';
                break;
            }
        }
        // Leaving Relay Race hands the first controller back to player 1.
        if(!relay && (sfr::relay_pad() || relay_tracking_id.load(std::memory_order_relaxed))) {
            sfr::set_relay_pad(0);
            relay_tracking_id.store(0,std::memory_order_relaxed);
        }
    }
    uint16_t spoken=pressed;
    // The recognizer hears one word a frame, from nobody in particular: in a
    // race the second pad's START pauses as the first's does, and its words
    // run the pause menu (the first pad's are heard the whole race as well).
    namespace pad=sfr::gamepad_button;
    if(racing) spoken|=uint16_t(second_pressed & (pad::start|pad::a|pad::b|pad::x|pad::dpad_up|pad::dpad_down));
    // A and X (and B for a yes/no dialog) of an open hand-pointer dialog are the
    // dialog's (823EF348 below). Spoken as well, one press both answered the
    // pause menu's Retry confirmation by voice and chose "yes" again: the
    // confirmation came back during the restart and stayed on screen.
    if(dialog_seen && input_frames-dialog_frame<=2)
        spoken&=uint16_t(~(sfr::gamepad_button::a | sfr::gamepad_button::x |
                           (dialog_layout==4 ? sfr::gamepad_button::b : 0)));
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
    // SFR_SAY_MIN_SECONDS=n: a word is also not said sooner than n seconds
    // after the one before (the first, after the program started). The menus
    // answer after loading and animations that take time on the wall clock,
    // not in presents; on a PC that presents them quickly a script by presents
    // alone speaks too early, and some runs never reach the race.
    static const double min_seconds=[]{ const char* t=std::getenv("SFR_SAY_MIN_SECONDS"); return t?std::strtod(t,nullptr):0.0; }();
    // SFR_SAY_REFERENCE_FPS=n: a word written "@P" is also not said before P/n
    // seconds since the start, so the script lasts as long on a PC that runs
    // at 60 fps as on one that runs at n (a PC slower than n is paced by the
    // presents as before).
    static const double reference_fps=[]{ const char* t=std::getenv("SFR_SAY_REFERENCE_FPS"); return t?std::strtod(t,nullptr):0.0; }();
    static const auto started=std::chrono::steady_clock::now();
    static auto last_said=started;
    static auto last_input=started;
    static size_t said_index=0;
    // A stall (a load with no frames) does not count toward min_seconds: the
    // menu was not answering during it. Counted on the wall clock alone, two
    // words came one present apart after a load and the second was lost
    // (skip-draws, where the menus run at hundreds of frames a second).
    if(said_index<script.size() && min_seconds>0.0) {
        const auto now=std::chrono::steady_clock::now();
        if(now-last_input>std::chrono::milliseconds(250)) last_said+=now-last_input;
        last_input=now;
    }
    // Ordinary play has no pending script: do not query the host clock there.
    if(said_index<script.size() && sfr::present_count>=script[said_index].first &&
       std::chrono::duration<double>(std::chrono::steady_clock::now()-last_said).count()>=min_seconds &&
       (reference_fps<=0.0 || std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()>=script[said_index].first/reference_fps)) {
        last_said=std::chrono::steady_clock::now();
        const auto& entry=script[said_index++];
        if(said_index==script.size()) sfr::say_done_present=sfr::present_count.load();
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
    // The Pause Gesture held (watch_pause_gesture): START in a race, which
    // the title hears as "pauseopen". Left for a later frame when a word
    // above was heard instead -- and dropped if by then a menu is up (a
    // spoken "pause" in the same frame opened it), where START would
    // answer the pause menu instead of opening it.
    if(pause_requested.exchange(false,std::memory_order_relaxed) && pause_gesture_active(racing)) spoken|=pad::start;
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
SFR_MENU_HOOK(sub_823EF348) {
    sfr::enter_function(ctx,"sub_823EF348",0x823EF348);
    auto& memory=*sfr::active_memory;
    const uint32_t dialog=ctx.r3.u32;
    const uint32_t layout=memory.load<uint32_t>(uint64_t(dialog)+124);
    if(ctx.r4.u32) { dialog_frame=input_frames; dialog_layout=layout; dialog_seen=true; }
    {
        static uint32_t last_dialog=0, last_layout=~0u;
        if(ctx.r4.u32 && (dialog!=last_dialog || layout!=last_layout)) {
            last_dialog=dialog; last_layout=layout;
            std::cerr << "NUI_DIALOG_SEEN dialog=0x" << std::hex << dialog << " layout=" << std::dec << layout
                      << " lr=0x" << std::hex << uint32_t(ctx.lr) << std::dec << '\n';
        }
    }
    const int slot=ctx.r4.u32 ? sfr::dialog_choice(pressed,layout) : -1;
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
// newly pressed D-pad left or right of a player's pad turns that player's
// ring one item when the hand did not, once per press: the first pad's for
// player 0 or 1, the second pad's for player 2. SFR_NUI_RING_TRACE=1 logs
// every step, the hand's and the pad's.
SFR_MENU_HOOK(sub_8246A6D0) {
    sfr::enter_function(ctx,"sub_8246A6D0",0x8246A6D0);
    const uint32_t ring=ctx.r3.u32;
    const uint32_t player=sfr::active_memory->load<uint32_t>(ring);
    const int64_t now=now_ms();
    if(now-ring_asked_ms.exchange(now,std::memory_order_relaxed)>1000)
        std::cerr << "NUI_RING_ACTIVE racing=" << (sfr::active_memory->load<uint32_t>(0x83E52F8C)!=0) << '\n';
    __imp__sub_8246A6D0(ctx,base);
    static const bool trace=[] {const char* text=std::getenv("SFR_NUI_RING_TRACE");return text && *text=='1';}();
    if(trace && ctx.r3.u32)
        std::cerr << "NUI_RING_STEP ring=0x" << std::hex << ring << std::dec << " player=" << player
                  << " hand=" << ctx.r3.s32 << '\n';
    if(player>2 || ctx.r3.u32) return;
    // In a race (the pause menu) the first player's ring takes either pad.
    uint16_t* buttons=player==2 ? &second_pressed : &pressed;
    int step=sfr::ring_step(*buttons);
    if(!step && player<2 && sfr::active_memory->load<uint32_t>(0x83E52F8C)!=0) {
        buttons=&second_pressed;
        step=sfr::ring_step(*buttons);
    }
    if(!step) return;
    *buttons&=~(sfr::gamepad_button::dpad_left|sfr::gamepad_button::dpad_right);
    ctx.r3.s64=step;
    if(trace)
        std::cerr << "NUI_RING_STEP ring=0x" << std::hex << ring << std::dec << " player=" << player
                  << " pad=" << step << '\n';
}

PPC_FUNC_IMPL(__imp__sub_82452F48);
SFR_MENU_HOOK(sub_82452F48) {
    sfr::enter_function(ctx,"sub_82452F48",0x82452F48);
    const uint32_t slot=ctx.r3.u32;
    __imp__sub_82452F48(ctx,base);
    sfr::NuiMenuServiceScope::record_wait(slot,(ctx.r3.u32 & 0xff)!=0);
}

PPC_FUNC_IMPL(__imp__sub_82456700);
SFR_MENU_HOOK(sub_82456700) {
    sfr::enter_function(ctx,"sub_82456700",0x82456700);
    // This updates the cursor before checking whether a pending animation
    // blocks its delayed action, so elapsed time alone cannot prove service.
    sfr::NuiMenuServiceScope::record(ctx.r3.u32,ctx.r4.u32);
    __imp__sub_82456700(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_824560A8);
// Observe the game's own hand activation result, logical-player binding and
// cursor state together. Opt-in diagnostics only; no gesture or input changes.
SFR_MENU_HOOK(sub_824560A8) {
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
            if(hands) line<<" cursor="<<number(cursor+40)<<','<<number(cursor+44);
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
SFR_MENU_HOOK(sub_824578F0) {
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
        if (player==0 && page) { menu_page_frame=input_frames; menu_page_now=page; }
        player_routing.observe_menu_page(player,page,page ? memory.load<uint32_t>(page+336) : 0,cursor_pending);
    }
    // SFR_MENU_DUMP=1 reports each menu page: its buttons as type/flags/kind/state
    // and the player's current page, whenever they change (a debugging aid).
    static const bool menu_dump=std::getenv("SFR_MENU_DUMP")!=nullptr;
    if(menu_dump) {
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
    // A two-player page hears no voice words. A of a player's pad confirms
    // the front item of that player's ring (+308 bit 0x8000) the way the
    // title's own confirm does (voice: 82464F34; the hand's arrow: 82456700):
    // the item goes into the player's pending slot (manager+128+4*player),
    // then 82460668(manager, item, player) takes it, which records it in
    // manager+136+4*player. Once a player has chosen, A does nothing more.
    if(sfr::second_player_pad().has_value() && memory.load<uint8_t>(0x83E515FB)) {
        for(uint32_t player=0; player<2; ++player) {
            uint16_t& buttons=player ? second_pressed : pressed;
            if(!(buttons & button::a)) continue;
            if(memory.load<uint32_t>(manager+136+4*player)) continue;
            const uint32_t page=call_guest(ctx,base,sub_82457348,manager,player);
            if(!page || ((memory.load<uint32_t>(page+320)>>4)&1)!=player) continue;
            uint32_t front=0;
            const uint32_t b0=memory.load<uint32_t>(page+300), b1=memory.load<uint32_t>(page+304);
            for(uint32_t slot=b0; slot<b1 && slot-b0<8*64 && !front; slot+=8)
                if(const uint32_t b=memory.load<uint32_t>(slot); b && (memory.load<uint32_t>(b+308)&0x8000)) front=b;
            if(!front) continue;
            buttons&=~button::a;
            memory.store<uint32_t>(manager+128+4*player,front);
            call_guest(ctx,base,sub_82460668,manager,front,player);
            std::cerr << "NUI_MENU_CONFIRM player=" << player << " type=" << memory.load<uint32_t>(front+288)
                      << " chosen=0x" << std::hex << memory.load<uint32_t>(manager+136+4*player) << std::dec << '\n';
        }
    }
    // B of a player's pad on a two-player page is that player's back
    // button (82456700, type 31: 8245B130 asks, 824603B8 leaves for the
    // player), the first pad for player 0 and the second for 1. As with the
    // arrow, the page goes back for both players, even after a choice.
    if(sfr::second_player_pad().has_value() && memory.load<uint8_t>(0x83E515FB)) {
        for(uint32_t player=0; player<2; ++player) {
            uint16_t& buttons=player ? second_pressed : pressed;
            if(!(buttons & button::b)) continue;
            const uint32_t page=call_guest(ctx,base,sub_82457348,manager,player);
            if(!page || ((memory.load<uint32_t>(page+320)>>4)&1)!=player) continue;
            buttons&=~button::b;
            if(call_guest(ctx,base,sub_8245B130,manager)&0xFF) {
                call_guest(ctx,base,sub_824603B8,manager,player);
                std::cerr << "NUI_MENU_BACK player=" << player << '\n';
            }
        }
    }
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
    // A on a page whose only button takes the hand (no voice word: the Quick
    // Match lobby entry, kind 2) presses it the way the title's own press
    // does (pending slot, then 82460668), like A on a two-player page.
    // SFR_HAND_SEEK=1 sends the emulated hand to it instead (the older way).
    static const bool hand_seek_enabled=[] { const char* t=std::getenv("SFR_HAND_SEEK"); return t && *t=='1'; }();
    if((pressed & button::a) && !hand_seek.button) {
        // The player's page: does it have a button A can say "ok" to?
        const uint32_t page=call_guest(ctx,base,sub_82457348,manager,0);
        uint32_t hand_only=0;
        bool spoken=false;
        if(page) {
            const uint32_t b0=memory.load<uint32_t>(page+300), b1=memory.load<uint32_t>(page+304);
            for(uint32_t slot=b0; slot<b1 && slot-b0<8*64; slot+=8) {
                const uint32_t b=memory.load<uint32_t>(slot);
                if(!b) continue;
                const uint32_t state=memory.load<uint32_t>(b+308);
                if(state & 0x800000) spoken=true;
                else if(memory.load<uint32_t>(b+312)==2 && !hand_only) hand_only=b;
            }
        }
        if(hand_only && !spoken && !hand_seek_enabled) {
            pressed&=~button::a;
            memory.store<uint32_t>(manager+128,hand_only);
            call_guest(ctx,base,sub_82460668,manager,hand_only,0);
            std::cerr << "NUI_MENU_PRESS button=0x" << std::hex << hand_only << std::dec
                      << " type=" << memory.load<uint32_t>(hand_only+288) << '\n';
        } else if(hand_only && !spoken) {
            hand_seek={hand_only,manager,0,0};
            std::cerr << "NUI_HAND_SEEK start button=0x" << std::hex << hand_only << std::dec
                      << " type=" << memory.load<uint32_t>(hand_only+288) << '\n';
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
SFR_CONCURRENT_HOOK(sub_82439530) {
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
    const uint32_t manager=ctx.r3.u32;
    const uint64_t given=depth_given_count.load(std::memory_order_acquire);
    __imp__sub_82439530(ctx,base);
    // SFR_KINECT_RECORD: when this run fetched a new depth image (its
    // 82438268 asks NuiImageStreamGetNextFrame, on this thread), what the
    // view computed from it for each player slot, for the race's rows
    // (kinect_race_record.h; the layout is kinect_depth_features.h's).
    if(depth_given_count.load(std::memory_order_acquire)!=given && !sfr::kinect_record_prefix().empty()) {
        auto& m=*sfr::active_memory;
        const uint64_t frame=depth_given_number.load(std::memory_order_relaxed);
        const int64_t now=sfr::kinect_record_now_us();
        const auto f32=[&](uint64_t address) { return std::bit_cast<float>(m.load<uint32_t>(address)); };
        for(uint32_t k=0;k<2;++k) {
            const int32_t slot=int32_t(m.load<uint32_t>(uint64_t(manager)+7740+4*k));
            if(slot<0 || slot>5) continue;
            const uint64_t at=uint64_t(manager)+8*uint32_t(slot);
            const uint64_t record=uint64_t(manager)+2784+800*uint32_t(slot);
            sfr::KinectDepthViewRecord view;
            view.known=true;
            view.frame=frame;
            view.steady_us=now;
            view.slot=uint32_t(slot);
            view.centroid.column=m.load<uint32_t>(at+7584);
            view.centroid.row=m.load<uint32_t>(at+7588);
            view.centroid.min_column=m.load<uint32_t>(at+7632);
            view.centroid.min_row=m.load<uint32_t>(at+7636);
            view.centroid.max_column=m.load<uint32_t>(at+7680);
            view.centroid.max_row=m.load<uint32_t>(at+7684);
            for(uint32_t i=0;i<4;++i) {
                view.counts[i]=f32(record+640+4*i);
                view.means[i]=f32(record+656+4*i);
            }
            view.near_count=m.load<uint32_t>(record+672);
            view.average=f32(record+676);
            // By the record, and by the slot (a record's +784, 82438A98) for a
            // race whose detectors are asked about another copy of it.
            sfr::kinect_depth_view_publish(uint32_t(record),view);
            sfr::kinect_depth_view_publish(uint32_t(slot),view);
        }
    }
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
SFR_MENU_HOOK(sub_82463808) {
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
// The pools' allocation and its two frees (824A3250, 824A3598, reached
// through the pool's table) under one lock of their own: run beside each
// other without one, they corrupted the free lists (the pool's own lock
// object does not exclude another thread), and under the global permit
// they were a third of what brought free-running guests back to it.
static std::recursive_mutex pool_lock;
static std::atomic<uint32_t> pool_holder{0};
struct PoolLock {
    // Taken before the lock: its holder keeps the permit until it lets go.
    sfr::NoHandoff keep_permit;
    PoolLock() {
        if(pool_lock.try_lock()) { pool_holder.store(sfr::current_guest_thread_id(),std::memory_order_relaxed); return; }
        const auto started=std::chrono::steady_clock::now();
        const uint32_t was=pool_holder.load(std::memory_order_relaxed);
        pool_lock.lock();
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        if(ms>=100) std::cerr<<"LOCK_LONG_WAIT lock=pool guest_id="<<sfr::current_guest_thread_id()<<" ms="<<ms<<" holder="<<was<<std::endl;
        pool_holder.store(sfr::current_guest_thread_id(),std::memory_order_relaxed);
    }
    ~PoolLock() { pool_lock.unlock(); }
};
PPC_FUNC_IMPL(__imp__sub_824A3250);
SFR_CONCURRENT_HOOK(sub_824A3250) {
    sfr::enter_function(ctx,"sub_824A3250",0x824A3250);
    PoolLock lock;
    __imp__sub_824A3250(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_824A3598);
SFR_CONCURRENT_HOOK(sub_824A3598) {
    sfr::enter_function(ctx,"sub_824A3598",0x824A3598);
    PoolLock lock;
    __imp__sub_824A3598(ctx,base);
}
SFR_CONCURRENT_HOOK(sub_824A3398) {
    sfr::enter_function(ctx,"sub_824A3398",0x824A3398);
    const uint32_t pool=ctx.r3.u32, size=ctx.r4.u32, alignment=ctx.r5.u32;
    {
        PoolLock lock;
        __imp__sub_824A3398(ctx,base);
    }
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
SFR_CONCURRENT_HOOK(sub_8243D1B8) {
    sfr::enter_function(ctx,"sub_8243D1B8",0x8243D1B8);
    static uint32_t calls=0;
    if(calls++<8)
        std::cerr << "NUI_MENU_FADE object=0x" << std::hex << ctx.r3.u32 << " a=" << std::dec << ctx.r4.u32
                  << " b=" << ctx.r5.u32 << " colour=0x" << std::hex << ctx.r6.u32 << std::dec << '\n';
    __imp__sub_8243D1B8(ctx,base);
}
