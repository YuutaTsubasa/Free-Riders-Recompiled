#include "race_stereo.h"

#include "diagnostic_hooks.h"
#include "guest_graphics.h"
#include "native_presentation.h"
#include "ppc_recomp_shared.h"
#include "stereo_view.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

// The race's stereo test view (Issue #460): a race frame drawn twice side by
// side, the left eye into the left half of the race's view and the right eye
// into the right half, for looking at without a headset (cross or parallel
// viewing) before a VR runtime is added.
//
// How the title draws a frame of N views: its task manager (824A00C8, hooked
// in race_task_hooks.cpp) runs the 3D drawing (phase key 4, each task's
// method 12) once per view. The layout manager's callback after each round
// (8222B940) switches the view with 8222B6F8 (the viewport from the layout's
// entry for it, the camera's projection for the scene graph) and the lights
// (82214698), and says whether to go round again; after the last view it
// selects the whole frame and the full-frame passes (phase key 8: the post
// chain, the composite and the HUD) run once. A two-player race draws its
// 3D twice a frame like this, so the title's drawing already copes with
// running twice. Here a one-view race does the same with one camera moved to
// each eye in turn:
//   - the view's layout entry ([layout+148], x/y/width/height and aspect)
//     becomes the eye's half, and the camera's viewport (+688 x, +692 width)
//     and aspect (+492) follow it;
//   - the camera's eye (+432) and target (+448) move by half the separation
//     along the view's right; its matrices are made again by its update
//     (824B0018) with its controller ([+496]) set aside, so the controller
//     does not step (that is the update the title runs once a frame);
//   - then what a two-player view switch does: 8222B6F8(layout, 0, force)
//     sets the viewport and the projection, the camera table's vt+8 gives
//     the renderer the camera's view and projection, and 82214698 the
//     lights.
// The view count (+152), the current view (+164) and the current camera
// ([cameras+28]) stay as they are: the race camera's controller and other
// logic read the count. After the second eye the camera and the layout entry
// are put back byte for byte, and the view state is set from them again, so
// the full-frame passes and the next frame's logic see the camera the title
// made. Menus, two-player races and anything not drawn stay as they are.
//
// SFR_TEST_FIRST_PERSON=1 moves the camera to the rider's head the same way
// (only while drawing; the logic keeps the title's camera), with or without
// stereo.

namespace sfr { void write_framebuffer(const char* path); }  // guest_graphics_hooks.cpp

PPC_FUNC(sub_8222B6F8);
PPC_FUNC(sub_82214698);
PPC_FUNC(sub_824B0018);

namespace sfr::race_stereo {
namespace {
constexpr uint32_t race_flag = 0x83E52F8C;
constexpr uint32_t scene_services = 0x83E516A0;
constexpr uint32_t layout_manager = 0x83E52F5C;
constexpr uint32_t light_manager = 0x83E52F68;
constexpr uint32_t racer_team = 0x83E52FDC;
constexpr uint32_t camera_update = 0x824B0018;
// The camera's fields this file reads or writes (see stereo_view.h): it is
// put back whole, 0x2C0 bytes, as far as the viewport's last float.
constexpr uint32_t camera_size = 0x2C0;
constexpr uint32_t camera_view = 32, camera_eye = 432, camera_target = 448, camera_up = 464, camera_aspect = 492,
                   camera_controller = 496, camera_viewport_x = 688, camera_viewport_width = 692;
constexpr uint32_t layout_entry_size = 28;
// The first-person eye: above the racer's position (at its feet) and ahead,
// out of the rider's own head (metres).
constexpr float head_height = 1.5f, head_ahead = 0.5f;

bool flag(const char* name) {
    const char* t = std::getenv(name);
    return t && *t == '1';
}
bool stereo_on() {
    static const bool on = flag("SFR_TEST_STEREO");
    return on;
}
bool first_person_on() {
    static const bool on = flag("SFR_TEST_FIRST_PERSON");
    return on;
}
float separation() {
    static const float value = stereo::parse_separation(std::getenv("SFR_TEST_STEREO_SEPARATION"));
    return value;
}
// SFR_TEST_STEREO_LOG=1 (testing): the camera and the chosen racer every
// 300 stereo frames, as STEREO_VIEW lines.
bool log_on() {
    static const bool on = flag("SFR_TEST_STEREO_LOG");
    return on;
}
// SFR_TEST_STEREO_ALTERNATE=1 (testing): every other presented frame is
// drawn as the title draws it, so a paused race (whose picture holds still)
// shows the same moment in stereo and in mono, frame after frame, to compare
// each eye with the middle of the mono picture.
bool alternate_mono() {
    static const bool on = flag("SFR_TEST_STEREO_ALTERNATE");
    return on && present_count.load(std::memory_order_relaxed) % 2 == 0;
}

float load_float(uint32_t address) { return std::bit_cast<float>(active_memory->load<uint32_t>(address)); }
void store_float(uint32_t address, float value) { active_memory->store<uint32_t>(address, std::bit_cast<uint32_t>(value)); }
stereo::Vec3 load_vec(uint32_t address) { return {load_float(address), load_float(address + 4), load_float(address + 8)}; }
void store_vec(uint32_t address, stereo::Vec3 v) {
    store_float(address, v.x);
    store_float(address + 4, v.y);
    store_float(address + 8, v.z);
}

struct Frame {
    bool active = false;
    uint32_t layout = 0, entry = 0, camera = 0, cameras = 0;
    std::array<uint8_t, camera_size> camera_bytes{};
    std::array<uint8_t, layout_entry_size> entry_bytes{};
    uint32_t width = 0, height = 0;
    float aspect = 0;
    stereo::Vec3 eye, target, up, right;
    int passes = 1;
};
Frame frame;
uint32_t stereo_frames = 0;

// The racer the camera follows: of the race's racers ([83E52FDC]+36), the
// one nearest the camera's target. 0 when there are fewer than two: the
// World Grand Prix's story scenes and loading already raise the race flag
// with one racer made (twelve once the race is set up).
uint32_t followed_racer(stereo::Vec3 target) {
    auto& memory = *active_memory;
    const uint32_t team = memory.readable(racer_team, 4) ? memory.load<uint32_t>(racer_team) : 0;
    if (!team || !memory.readable(team + 36, 8)) return 0;
    const uint32_t begin = memory.load<uint32_t>(team + 36), end = memory.load<uint32_t>(team + 40);
    if (end <= begin + 4 || end - begin > 64 * 4) return 0;
    uint32_t best = 0;
    float best_distance = 0;
    for (uint32_t slot = begin; slot < end; slot += 4) {
        const uint32_t racer = memory.load<uint32_t>(slot);
        if (!racer || !memory.readable(racer, 0x200)) continue;
        const float distance = stereo::length(load_vec(racer + 0x120) - target);
        if (!best || distance < best_distance) best = racer, best_distance = distance;
    }
    return best;
}

void log_view(uint32_t racer) {
    char text[1024];
    int n = std::snprintf(text, sizeof text,
                          "STEREO_VIEW frame=%u camera=0x%x eye=%.3f,%.3f,%.3f target=%.3f,%.3f,%.3f up=%.3f,%.3f,%.3f "
                          "right=%.3f,%.3f,%.3f aspect=%.4f fov=%.4f near=%.4f far=%.1f view=%u+%u racer=0x%x rows=",
                          stereo_frames, frame.camera, frame.eye.x, frame.eye.y, frame.eye.z, frame.target.x,
                          frame.target.y, frame.target.z, frame.up.x, frame.up.y, frame.up.z, frame.right.x,
                          frame.right.y, frame.right.z, frame.aspect, load_float(frame.camera + 480),
                          load_float(frame.camera + 484), load_float(frame.camera + 488), 0u, frame.width, racer);
    if (racer)
        for (uint32_t offset = 0xC0; offset < 0x130 && n > 0 && n < int(sizeof text) - 16; offset += 4)
            n += std::snprintf(text + n, sizeof text - n, "%.3f,", load_float(racer + offset));
    const uint32_t table = active_memory->load<uint32_t>(frame.cameras);
    std::cerr << text << " cameras_vtable=0x" << std::hex << table << " vt8=0x" << active_memory->load<uint32_t>(table + 8)
              << " camera_vtable=0x" << active_memory->load<uint32_t>(frame.camera) << " entry=" << std::dec;
    for (uint32_t offset = 0; offset < layout_entry_size; offset += 4)
        std::cerr << active_memory->load<uint32_t>(frame.entry + offset) << ',';
    std::cerr << " proj=";
    for (uint32_t offset = 224; offset < 288; offset += 4) std::cerr << load_float(frame.camera + offset) << ',';
    std::cerr << " proj_ptr=0x" << std::hex << active_memory->load<uint32_t>(frame.camera + 416) << std::dec << '\n';
}

// The first-person pose: at the head of the racer the camera follows (its
// position +0x120; the title's units are metres: the chase camera stands
// about 3.5 behind and 1.9 above it), looking the way the title's chase
// camera looks (it follows the board's heading). The racer's own rotation is
// not used: where it is kept was not found (+0xD0 and +0xE0 stay at the
// identity quaternion while it turns).
bool first_person_pose(stereo::Pose& pose) {
    const uint32_t racer = followed_racer(frame.target);
    if (!racer) return false;
    const stereo::Vec3 forward = frame.target - frame.eye;
    if (stereo::length(forward) < 1e-3f) return false;
    pose = stereo::head_pose(load_vec(racer + 0x120), forward, frame.up, head_height, head_ahead);
    return true;
}

// The camera's matrices again from its fields, as its update does, without
// stepping its controller.
void remake_camera(PPCContext& ctx, uint8_t* base) {
    auto& memory = *active_memory;
    const uint32_t controller = memory.load<uint32_t>(frame.camera + camera_controller);
    memory.store<uint32_t>(frame.camera + camera_controller, 0);
    ctx.r3.u64 = frame.camera;
    sub_824B0018(ctx, base);
    memory.store<uint32_t>(frame.camera + camera_controller, controller);
}

// The view's layout entry: x, y, width, height (the frame's height).
void set_entry(uint32_t x, uint32_t width) {
    auto& memory = *active_memory;
    memory.store<uint32_t>(frame.entry, x);
    memory.store<uint32_t>(frame.entry + 4, 0);
    memory.store<uint32_t>(frame.entry + 8, width);
    memory.store<uint32_t>(frame.entry + 12, frame.height);
}

// What a two-player view switch does for view 0, forced (8222B6F8 leaves a
// one-view layout alone), and the lights for it.
void select_view(PPCContext& ctx, uint8_t* base) {
    ctx.r3.u64 = frame.layout;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 1;
    sub_8222B6F8(ctx, base);
    // The camera table's vt+8 (824B1800): hands the current camera's view
    // (+32) and projection ([+416]) to the renderer ([82211440()+60] vt+20),
    // as the title does after switching views.
    ctx.r3.u64 = frame.cameras;
    sfr::call_indirect(ctx, base, active_memory->load<uint32_t>(active_memory->load<uint32_t>(frame.cameras) + 8));
    ctx.r3.u64 = active_memory->load<uint32_t>(light_manager);
    sub_82214698(ctx, base);
}
}

bool enabled() { return stereo_on() || first_person_on(); }

namespace {
bool start(PPCContext& ctx, uint8_t* base);
}

bool begin(PPCContext& ctx, uint8_t* base) {
    const bool active = start(ctx, base);
    // With SFR_TEST_STEREO_LOG=1, each change between drawn as the title
    // draws it and drawn here, with the present.
    static bool was = false;
    if (log_on() && active != was) {
        std::cerr << "STEREO_ACTIVE " << active << " present=" << present_count.load(std::memory_order_relaxed) << '\n';
        was = active;
    }
    return active;
}

namespace {
bool start(PPCContext& ctx, uint8_t* base) {
    frame.active = false;
    if (!enabled() || !active_memory || alternate_mono()) return false;
    auto& memory = *active_memory;
    if (!memory.load<uint32_t>(race_flag)) return false;
    const uint32_t layout = memory.load<uint32_t>(layout_manager);
    const uint32_t scene = memory.load<uint32_t>(scene_services);
    if (!layout || !scene || memory.load<uint32_t>(layout + 152) != 1 || memory.load<uint32_t>(layout + 164) != 0)
        return false;
    const uint32_t cameras = memory.load<uint32_t>(scene + 116);
    if (!cameras || memory.load<uint32_t>(cameras + 28) != 0) return false;
    const uint32_t list = memory.load<uint32_t>(cameras + 8);
    const uint32_t camera = list ? memory.load<uint32_t>(list) : 0;
    const uint32_t entry = memory.load<uint32_t>(layout + 148);
    if (!camera || !entry || !memory.readable(camera, camera_size) || !memory.readable(entry, layout_entry_size)) return false;
    if (!active_guest_graphics || !active_guest_graphics->created()) return false;
    // Only the race itself, with its racers (followed_racer): the race flag
    // is up already on the World Grand Prix's story scenes and loading, and
    // one run drawing a story scene twice stopped on a heap block freed with
    // a broken header (82497DF8, guest 0x01000000).
    if (!followed_racer(load_vec(camera + camera_target))) return false;
    const uint32_t vtable = memory.load<uint32_t>(camera);
    if (memory.load<uint32_t>(vtable + 4) != camera_update) {
        static bool told = false;
        if (!told) std::cerr << "STEREO_VIEW unknown camera vtable=0x" << std::hex << vtable << std::dec << ": drawn as it is\n";
        told = true;
        return false;
    }
    frame.layout = layout;
    frame.entry = entry;
    frame.camera = camera;
    frame.cameras = cameras;
    std::memcpy(frame.camera_bytes.data(), base + camera, camera_size);
    std::memcpy(frame.entry_bytes.data(), base + entry, layout_entry_size);
    // A one-view race sets no viewport of its own (8222B6F8 leaves a
    // one-view layout alone; its entry says 880, the layout the console
    // scales to 16:9): it draws into the whole frame, whose viewport its
    // render target set. The eyes split that frame.
    frame.width = active_guest_graphics->presentation().width();
    frame.height = active_guest_graphics->presentation().height();
    if (frame.width < 2 || !frame.height) return false;
    frame.aspect = load_float(camera + camera_aspect);
    frame.eye = load_vec(camera + camera_eye);
    frame.target = load_vec(camera + camera_target);
    frame.up = load_vec(camera + camera_up);
    float view[16];
    for (int i = 0; i < 16; ++i) view[i] = load_float(camera + camera_view + 4 * i);
    frame.right = stereo::view_right(view);
    if (first_person_on()) {
        stereo::Pose pose;
        if (first_person_pose(pose)) {
            frame.eye = pose.eye;
            frame.target = pose.target;
            frame.up = pose.up;
            // The right of the view made from the pose (whichever hand the
            // title's look-at is), then the camera as it was.
            store_vec(camera + camera_eye, frame.eye);
            store_vec(camera + camera_target, frame.target);
            store_vec(camera + camera_up, frame.up);
            remake_camera(ctx, base);
            for (int i = 0; i < 16; ++i) view[i] = load_float(camera + camera_view + 4 * i);
            frame.right = stereo::view_right(view);
            std::memcpy(base + camera, frame.camera_bytes.data(), camera_size);
        }
    }
    frame.passes = stereo_on() ? stereo::eye_count : 1;
    if (frame.passes == stereo::eye_count) active_guest_graphics->set_frame_views(true);
    if (log_on() && stereo_frames % 300 == 0) log_view(followed_racer(load_vec(camera + camera_target)));
    ++stereo_frames;
    frame.active = true;
    return true;
}
}

int passes() { return frame.active ? frame.passes : 1; }

// SFR_TEST_STEREO_SHOT=<directory> (testing, with SFR_TEST_STEREO=1): every
// SFR_TEST_STEREO_SHOT_EVERY-th drawn race frame (300), the 3D is
// drawn once more first, as the title draws it, and the framebuffer is
// written after it (mono-<present>.bmp) and after the eyes
// (stereo-<present>.bmp), before the post effects and the HUD. The eyes
// then cover the whole frame again. With no separation each eye should be
// the middle of the mono picture, pixel for pixel.
bool shot_due() {
    static const char* const directory = std::getenv("SFR_TEST_STEREO_SHOT");
    static const uint32_t every = [] {
        const char* t = std::getenv("SFR_TEST_STEREO_SHOT_EVERY");
        const long n = t ? std::strtol(t, nullptr, 10) : 300;
        return uint32_t(n > 0 ? n : 300);
    }();
    static uint32_t frames = 0;
    // Race frames only (the readback stalls a frame; on the story scenes,
    // timed by the clock, that made the test route miss its SKIP).
    if (!directory || !stereo_on() || !active_memory || !followed_racer({})) return false;
    return ++frames % every == 0;
}
void shot(const char* what) {
    char path[1024];
    std::snprintf(path, sizeof path, "%s/%s-%u.bmp", std::getenv("SFR_TEST_STEREO_SHOT"), what,
                  present_count.load(std::memory_order_relaxed));
    write_framebuffer(path);
    std::cerr << "STEREO_SHOT " << path << '\n';
}

void pass(PPCContext& ctx, uint8_t* base, int index) {
    if (!frame.active) return;
    auto& memory = *active_memory;
    std::memcpy(base + frame.camera, frame.camera_bytes.data(), camera_size);
    stereo::Vec3 offset{};
    if (frame.passes == stereo::eye_count) {
        const stereo::Span span = stereo::eye_span(0, frame.width, index);
        set_entry(span.x, span.width);
        store_float(frame.camera + camera_viewport_x, float(span.x));
        store_float(frame.camera + camera_viewport_width, float(span.width));
        store_float(frame.camera + camera_aspect, stereo::eye_aspect(frame.aspect, frame.width, span.width));
        offset = stereo::eye_offset(frame.right, separation(), index);
    } else {
        set_entry(0, frame.width);
    }
    store_vec(frame.camera + camera_eye, frame.eye + offset);
    store_vec(frame.camera + camera_target, frame.target + offset);
    store_vec(frame.camera + camera_up, frame.up);
    remake_camera(ctx, base);
    select_view(ctx, base);
}

void end(PPCContext& ctx, uint8_t* base) {
    if (!frame.active) return;
    // The camera as the title left it, the view state from it again, and the
    // whole frame's viewport, as the title's one-view drawing has it; then
    // the layout entry as it was.
    std::memcpy(base + frame.camera, frame.camera_bytes.data(), camera_size);
    set_entry(0, frame.width);
    select_view(ctx, base);
    std::memcpy(base + frame.entry, frame.entry_bytes.data(), layout_entry_size);
    active_guest_graphics->set_frame_views(false);
    frame.active = false;
}
}
