#include "kinect_sensor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <type_traits>

// The Kinect for Windows runtime 1.8, loaded when asked for rather than
// linked: a player without a sensor (nearly everyone) needs neither the SDK
// to build nor Kinect10.dll to start. The declarations below are the few of
// NuiApi.h / NuiSkeleton.h this uses, with their layout.

namespace sfr {
namespace {

struct NuiVector4 { float x, y, z, w; };

constexpr int nui_skeleton_count = 6;
constexpr int nui_position_count = 20;

struct NuiSkeletonData {
    int32_t tracking_state;  // NUI_SKELETON_TRACKING_STATE: 2 tracked, 1 position only
    DWORD tracking_id;
    DWORD enrollment_index;
    DWORD user_index;
    NuiVector4 position;
    NuiVector4 positions[nui_position_count];
    int32_t position_states[nui_position_count];  // 0 not tracked, 1 inferred, 2 tracked
    DWORD quality_flags;
};

struct NuiSkeletonFrame {
    LARGE_INTEGER timestamp;
    DWORD frame_number;
    DWORD flags;
    NuiVector4 floor_clip_plane;
    NuiVector4 normal_to_gravity;
    NuiSkeletonData skeletons[nui_skeleton_count];
};

struct NuiTransformSmoothParameters {
    float smoothing, correction, prediction, jitter_radius, max_deviation_radius;
};

static_assert(sizeof(NuiSkeletonData) == 436, "NUI_SKELETON_DATA layout");
static_assert(sizeof(NuiSkeletonFrame) == 2664, "NUI_SKELETON_FRAME layout");

constexpr DWORD nui_initialize_flag_uses_skeleton = 0x00000008;
constexpr int32_t nui_skeleton_tracked = 2;

using NuiGetSensorCount = HRESULT(WINAPI*)(int*);
using NuiInitialize = HRESULT(WINAPI*)(DWORD);
using NuiShutdown = void(WINAPI*)();
using NuiSkeletonTrackingEnable = HRESULT(WINAPI*)(HANDLE, DWORD);
using NuiSkeletonGetNextFrame = HRESULT(WINAPI*)(DWORD, NuiSkeletonFrame*);
using NuiTransformSmooth = HRESULT(WINAPI*)(NuiSkeletonFrame*, const NuiTransformSmoothParameters*);

std::string hresult(HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(value));
    return text;
}

class WindowsKinect final : public KinectSensor {
public:
    ~WindowsKinect() override {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        if (initialized_) shutdown_();
        if (event_) CloseHandle(event_);
        if (library_) FreeLibrary(library_);
    }

    const char* model() const override { return "Kinect v1"; }

    bool start(std::string* why) {
        library_ = LoadLibraryW(L"Kinect10.dll");
        if (!library_) { if (why) *why = "no-runtime"; return false; }
        const auto get = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                reinterpret_cast<void*>(GetProcAddress(library_, name)));
            return function != nullptr;
        };
        NuiGetSensorCount count = nullptr;
        NuiInitialize initialize = nullptr;
        NuiSkeletonTrackingEnable enable = nullptr;
        if (!get(count, "NuiGetSensorCount") || !get(initialize, "NuiInitialize") || !get(shutdown_, "NuiShutdown") ||
            !get(enable, "NuiSkeletonTrackingEnable") || !get(next_frame_, "NuiSkeletonGetNextFrame")) {
            if (why) *why = "incomplete-runtime";
            return false;
        }
        get(smooth_, "NuiTransformSmooth");  // optional: frames go unsmoothed without it
        int sensors = 0;
        if (FAILED(count(&sensors)) || sensors < 1) { if (why) *why = "no-sensor"; return false; }
        // SFR_KINECT_V1_FLAGS (NuiInitialize's flags, default skeleton only)
        // and SFR_KINECT_V1_READ ("event": read when the frame event fires;
        // "wait": let NuiSkeletonGetNextFrame wait for the frame) are there
        // to find, on a sensor that signals frames but hands over no data
        // (E_NUI_FRAME_NO_DATA), which way it does hand them over.
        DWORD flags = nui_initialize_flag_uses_skeleton;
        if (const char* text = std::getenv("SFR_KINECT_V1_FLAGS"); text && *text)
            flags = DWORD(std::strtoul(text, nullptr, 0)) | nui_initialize_flag_uses_skeleton;
        const char* read = std::getenv("SFR_KINECT_V1_READ");
        wait_in_sdk_ = read && std::string_view(read) == "wait";
        std::cerr << "NATIVE_KINECT_OPEN model=v1 flags=0x" << std::hex << flags << std::dec
                  << " read=" << (wait_in_sdk_ ? "wait" : "event") << std::endl;
        if (const HRESULT result = initialize(flags); FAILED(result)) {
            // E_NUI_DEVICE_NOT_READY and the like: unpowered, or in use.
            if (why) *why = "initialize-" + hresult(result);
            return false;
        }
        initialized_ = true;
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event_) { if (why) *why = "no-event"; return false; }
        if (const HRESULT result = enable(event_, 0); FAILED(result)) {
            if (why) *why = "tracking-" + hresult(result);
            return false;
        }
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        return true;
    }

    bool next(KinectFrame& frame) override {
        std::lock_guard guard(lock_);
        if (latest_.number == taken_) return false;
        frame = latest_;
        taken_ = latest_.number;
        return true;
    }

private:
    void run(std::stop_token stop) {
        // The SDK's suggested middle ground: most of the jitter gone for
        // about a frame of latency, which a racing game can afford.
        constexpr NuiTransformSmoothParameters smoothing{0.5f, 0.5f, 0.5f, 0.05f, 0.04f};
        NuiSkeletonFrame raw{};
        KinectFrame frame;
        uint64_t frames = 0, tracked = 0, waits = 0, failures = 0;
        HRESULT last_failure = S_OK;
        auto reported = std::chrono::steady_clock::now();
        // Every five seconds, frames or not: a sensor that opens but sends
        // nothing (in use by another program, short of power, on a USB
        // controller it does not get on with) says so instead of staying
        // silent.
        const auto report = [&] {
            const auto now = std::chrono::steady_clock::now();
            if (now - reported < std::chrono::seconds(5)) return;
            std::cerr << "NATIVE_KINECT model=v1 frames=" << frames << " with_body=" << tracked
                      << " bodies=" << frame.bodies.size() << " empty_waits=" << waits << " failed=" << failures;
            if (failures) std::cerr << " last_error=" << hresult(last_failure);
            if (!frames && !failures) std::cerr << " (no skeleton frames: close any other program using the Kinect)";
            std::cerr << std::endl;
            reported = now;
            frames = tracked = waits = failures = 0;
        };
        while (!stop.stop_requested()) {
            report();
            if (!wait_in_sdk_) {
                if (WaitForSingleObject(event_, 100) != WAIT_OBJECT_0) { ++waits; continue; }
                ResetEvent(event_);
            }
            if (const HRESULT result = next_frame_(wait_in_sdk_ ? 100 : 0, &raw); FAILED(result)) {
                ++failures;
                last_failure = result;
                continue;
            }
            if (smooth_) smooth_(&raw, &smoothing);
            frame.floor_plane = {raw.floor_clip_plane.x, raw.floor_clip_plane.y, raw.floor_clip_plane.z,
                                 raw.floor_clip_plane.w};
            frame.gravity = {raw.normal_to_gravity.x, raw.normal_to_gravity.y, raw.normal_to_gravity.z};
            frame.bodies.clear();
            for (const auto& skeleton : raw.skeletons) {
                if (skeleton.tracking_state != nui_skeleton_tracked || !skeleton.tracking_id) continue;
                KinectBody& body = frame.bodies.emplace_back();
                body.tracking_id = skeleton.tracking_id;
                body.position = {skeleton.position.x, skeleton.position.y, skeleton.position.z};
                for (int j = 0; j < nui_position_count; ++j) {
                    const auto& p = skeleton.positions[j];
                    body.joints[j] = {p.x, p.y, p.z};
                    body.joint_states[j] = uint32_t(skeleton.position_states[j] < 0 ? 0 : skeleton.position_states[j]);
                }
            }
            ++frames;
            if (!frame.bodies.empty()) ++tracked;
            {
                std::lock_guard guard(lock_);
                frame.number = latest_.number + 1;
                latest_ = frame;
            }
        }
    }

    HMODULE library_ = nullptr;
    HANDLE event_ = nullptr;
    bool initialized_ = false;
    bool wait_in_sdk_ = false;
    NuiShutdown shutdown_ = nullptr;
    NuiSkeletonGetNextFrame next_frame_ = nullptr;
    NuiTransformSmooth smooth_ = nullptr;
    std::mutex lock_;
    KinectFrame latest_;
    uint64_t taken_ = 0;
    std::jthread worker_;
};

}

std::unique_ptr<KinectSensor> open_kinect_v2(std::string* why);  // kinect_v2_win32.cpp

bool KinectSensor::supported() { return true; }

std::unique_ptr<KinectSensor> KinectSensor::open(std::string* why) {
    // The Xbox 360 kind first: its runtime answers at once whether a sensor
    // is there, where v2's takes a moment to tell.
    std::string v1_why, v2_why;
    if (auto sensor = std::make_unique<WindowsKinect>(); sensor->start(&v1_why)) return sensor;
    if (auto sensor = open_kinect_v2(&v2_why)) return sensor;
    if (why) *why = kinect_open_failure(v1_why, v2_why);
    return nullptr;
}

}
