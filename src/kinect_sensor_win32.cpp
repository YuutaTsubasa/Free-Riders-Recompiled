#include "kinect_sensor.h"
#include "kinect_record.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>
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

constexpr DWORD nui_initialize_flag_uses_depth_and_player_index = 0x00000001;
constexpr DWORD nui_initialize_flag_uses_color = 0x00000002;
constexpr DWORD nui_initialize_flag_uses_skeleton = 0x00000008;

// NuiImageFrame.h / NuiImageCamera.h: the image streams, the same types and
// resolutions as the console's (type 0 depth and player index, 1 colour;
// resolution 1 is 320x240, 2 is 640x480).
struct NuiLockedRect {
    INT pitch;
    INT size;
    BYTE* bits;
};
struct INuiFrameTexture : IUnknown {
    virtual int STDMETHODCALLTYPE BufferLen() = 0;
    virtual int STDMETHODCALLTYPE Pitch() = 0;
    virtual HRESULT STDMETHODCALLTYPE LockRect(UINT level, NuiLockedRect* locked, RECT* rect, DWORD flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, void* description) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) = 0;
};
struct NuiImageViewArea { int zoom; LONG center_x, center_y; };
struct NuiImageFrame {
    LARGE_INTEGER timestamp;
    DWORD frame_number;
    int32_t image_type;
    int32_t resolution;
    INuiFrameTexture* texture;
    DWORD frame_flags;
    NuiImageViewArea view_area;
};
using NuiImageStreamOpen = HRESULT(WINAPI*)(int32_t, int32_t, DWORD, DWORD, HANDLE, HANDLE*);
using NuiImageStreamGetNextFrame = HRESULT(WINAPI*)(HANDLE, DWORD, const NuiImageFrame**);
using NuiImageStreamReleaseFrame = HRESULT(WINAPI*)(HANDLE, const NuiImageFrame*);
using NuiCameraElevationGetAngle = HRESULT(WINAPI*)(LONG*);
using NuiCameraElevationSetAngle = HRESULT(WINAPI*)(LONG);
constexpr LONG nui_camera_elevation_limit = 27;

struct ImageStream {
    int32_t type, resolution;
    uint32_t width, height, bytes_per_pixel;
    HANDLE handle = nullptr;
};
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
    std::string depth_failure() const override { return depth_failure_; }
    bool has_depth_stream() const override { return streams_[0].handle != nullptr; }
    bool has_colour_stream() const override { return streams_[1].handle != nullptr; }

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
        get(elevation_get_, "NuiCameraElevationGetAngle");  // optional too: the tilt motor
        get(elevation_set_, "NuiCameraElevationSetAngle");
        int sensors = 0;
        if (FAILED(count(&sensors)) || sensors < 1) { if (why) *why = "no-sensor"; return false; }
        // Frames are read by letting NuiSkeletonGetNextFrame wait for them.
        // Reading as soon as the frame event fires (SFR_KINECT_V1_READ=event)
        // got E_NUI_FRAME_NO_DATA on every frame from a Kinect for Xbox 360
        // (SDK 1.8, Windows 10), while waiting in the SDK got about 28 frames
        // a second. SFR_KINECT_V1_FLAGS adds NuiInitialize flags, for trying
        // another sensor.
        DWORD flags = nui_initialize_flag_uses_skeleton;
        if (const char* text = std::getenv("SFR_KINECT_V1_FLAGS"); text && *text)
            flags = DWORD(std::strtoul(text, nullptr, 0)) | nui_initialize_flag_uses_skeleton;
        // The depth (with the player index) and colour cameras as well, for
        // the title's own depth view and camera image, always: the race reads
        // the title's depth detectors, as on the console.
        const bool with_images = get(image_open_, "NuiImageStreamOpen") && get(image_next_, "NuiImageStreamGetNextFrame") &&
                                 get(image_release_, "NuiImageStreamReleaseFrame");
        if (!with_images) depth_failure_ = "no-image-api";
        if (with_images) flags |= nui_initialize_flag_uses_depth_and_player_index | nui_initialize_flag_uses_color;
        const char* read = std::getenv("SFR_KINECT_V1_READ");
        wait_in_sdk_ = !(read && std::string_view(read) == "event");
        std::cerr << "NATIVE_KINECT_OPEN model=v1 flags=0x" << std::hex << flags << std::dec
                  << " read=" << (wait_in_sdk_ ? "wait" : "event") << " smooth=" << (smooth_frames_ && smooth_ ? 1 : 0)
                  << std::endl;
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
        if (with_images)
            for (ImageStream& stream : streams_) {
                const HRESULT result = image_open_(stream.type, stream.resolution, 0, 2, nullptr, &stream.handle);
                if (FAILED(result)) {
                    stream.handle = nullptr;
                    if (stream.type == 0) depth_failure_ = "stream-" + hresult(result);
                }
                std::cerr << "NATIVE_KINECT_IMAGE_OPEN type=" << stream.type << " width=" << stream.width
                          << " height=" << stream.height << " result=" << hresult(result) << std::endl;
            }
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        return true;
    }

    bool elevation(int& degrees) override {
        LONG angle = 0;
        if (!elevation_get_ || FAILED(elevation_get_(&angle))) return false;
        degrees = int(angle);
        return true;
    }

    bool set_elevation(int degrees) override {
        if (!elevation_set_) return false;
        const LONG angle = std::clamp(LONG(degrees), -nui_camera_elevation_limit, nui_camera_elevation_limit);
        const HRESULT result = elevation_set_(angle);
        std::cerr << "NATIVE_KINECT_ELEVATION set=" << angle << " result=" << hresult(result) << std::endl;
        return SUCCEEDED(result);
    }

    bool image(KinectImageKind kind, KinectImage& out) override {
        std::lock_guard guard(lock_);
        const KinectImage& latest = images_[size_t(kind)];
        if (latest.number == 0 || latest.number == out.number) return false;
        out = latest;
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
        // SFR_KINECT_RECORD (kinect_record.h): every frame, before smoothing.
        const bool recording = kinect_record_skeletons() != nullptr;
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
            if (streams_[0].handle || streams_[1].handle)
                std::cerr << " depth_frames=" << image_frames_[0].exchange(0) << " colour_frames=" << image_frames_[1].exchange(0);
            std::cerr << kinect_record_queue_status();  // recordings dropped for a slow disk, queued
            if (!frames && !failures) std::cerr << " (no skeleton frames: close any other program using the Kinect)";
            std::cerr << std::endl;
            reported = now;
            frames = tracked = waits = failures = 0;
        };
        while (!stop.stop_requested()) {
            report();
            read_images();
            if (!wait_in_sdk_) {
                if (WaitForSingleObject(event_, 100) != WAIT_OBJECT_0) { ++waits; continue; }
                ResetEvent(event_);
            }
            if (const HRESULT result = next_frame_(wait_in_sdk_ ? 100 : 0, &raw); FAILED(result)) {
                ++failures;
                last_failure = result;
                // Disconnection can fail immediately even with an SDK timeout.
                // Bound retries so this worker cannot spin on that error.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            frame.captured_at = std::chrono::steady_clock::now();
            if (recording) record(raw);
            if (smooth_ && smooth_frames_) smooth_(&raw, &smoothing);
            frame.floor_plane = {raw.floor_clip_plane.x, raw.floor_clip_plane.y, raw.floor_clip_plane.z,
                                 raw.floor_clip_plane.w};
            frame.gravity = {raw.normal_to_gravity.x, raw.normal_to_gravity.y, raw.normal_to_gravity.z};
            frame.bodies.clear();
            for (uint32_t index = 0; index < uint32_t(nui_skeleton_count); ++index) {
                const auto& skeleton = raw.skeletons[index];
                if (skeleton.tracking_state != nui_skeleton_tracked || !skeleton.tracking_id) continue;
                KinectBody& body = frame.bodies.emplace_back();
                body.tracking_id = skeleton.tracking_id;
                body.sensor_index = index;
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

    // One skeleton frame as the SDK gave it, into the recording: its own
    // frame number and timestamp (liTimeStamp, milliseconds), the floor and
    // gravity, every fully tracked skeleton, and the newest depth image's
    // share of pixels without a depth.
    void record(const NuiSkeletonFrame& raw) {
        KinectRecordFrame out;
        out.steady_us = kinect_record_now_us();
        out.sensor = "v1";
        out.frame = raw.frame_number;
        out.sdk_time = raw.timestamp.QuadPart;
        out.floor = {raw.floor_clip_plane.x, raw.floor_clip_plane.y, raw.floor_clip_plane.z, raw.floor_clip_plane.w};
        out.gravity = {raw.normal_to_gravity.x, raw.normal_to_gravity.y, raw.normal_to_gravity.z};
        {
            std::lock_guard guard(lock_);
            if (depth_quality_frame_ && out.steady_us - depth_quality_us_ <= 1000000) {
                out.depth_invalid = depth_invalid_;
                out.depth_frame = depth_quality_frame_;
                out.depth_steady_us = depth_quality_us_;
            }
        }
        for (uint32_t index = 0; index < uint32_t(nui_skeleton_count); ++index) {
            const auto& skeleton = raw.skeletons[index];
            if (skeleton.tracking_state != nui_skeleton_tracked || !skeleton.tracking_id) continue;
            KinectRecordBody& body = out.bodies.emplace_back();
            body.sensor_index = index;
            body.tracking_id = skeleton.tracking_id;
            body.has_position = true;
            body.position = {skeleton.position.x, skeleton.position.y, skeleton.position.z};
            std::array<std::array<float, 3>, 20> joints{};
            std::array<int, 20> states{};
            for (int j = 0; j < nui_position_count; ++j) {
                joints[j] = {skeleton.positions[j].x, skeleton.positions[j].y, skeleton.positions[j].z};
                states[j] = skeleton.position_states[j];
            }
            kinect_record_from_v1(joints, states, body);
        }
        kinect_record_skeleton_frame(out);
    }

    // The newest frame of each open camera, if one has come since the last
    // look: the SDK keeps two, so reading at the skeleton's pace keeps up.
    void read_images() {
        for (size_t i = 0; i < streams_.size(); ++i) {
            ImageStream& stream = streams_[i];
            if (!stream.handle) continue;
            const NuiImageFrame* frame = nullptr;
            if (FAILED(image_next_(stream.handle, 0, &frame)) || !frame) continue;
            NuiLockedRect locked{};
            if (frame->texture && SUCCEEDED(frame->texture->LockRect(0, &locked, nullptr, 0)) && locked.bits) {
                const uint32_t row = stream.width * stream.bytes_per_pixel;
                if (locked.pitch >= INT(row)) {
                    scratch_.resize(size_t(row) * stream.height);
                    for (uint32_t y = 0; y < stream.height; ++y)
                        std::memcpy(scratch_.data() + size_t(y) * row, locked.bits + size_t(y) * locked.pitch, row);
                    // The depth image's share of pixels without a depth, for
                    // a recording (another sensor's light shows there first).
                    float invalid = -1;
                    if (i == 0 && stream.bytes_per_pixel == 2)
                        invalid = kinect_depth_invalid_ratio(reinterpret_cast<const uint16_t*>(scratch_.data()),
                                                             size_t(stream.width) * stream.height, 3);
                    std::lock_guard guard(lock_);
                    if (invalid >= 0) {
                        depth_invalid_ = invalid;
                        ++depth_quality_frame_;
                        depth_quality_us_ = kinect_record_now_us();
                    }
                    KinectImage& latest = images_[i];
                    latest.width = stream.width;
                    latest.height = stream.height;
                    latest.bytes_per_pixel = stream.bytes_per_pixel;
                    latest.pixels.swap(scratch_);
                    // Its clocks, for a depth recording (kinect_record_depth.h).
                    latest.steady_us = kinect_record_now_us();
                    latest.sdk_time = frame->timestamp.QuadPart;
                    latest.sdk_frame = frame->frame_number;
                    ++latest.number;
                    ++image_frames_[i];
                }
                frame->texture->UnlockRect(0);
            }
            image_release_(stream.handle, frame);
        }
    }

    HMODULE library_ = nullptr;
    HANDLE event_ = nullptr;
    bool initialized_ = false;
    bool wait_in_sdk_ = true;
    NuiShutdown shutdown_ = nullptr;
    NuiSkeletonGetNextFrame next_frame_ = nullptr;
    NuiTransformSmooth smooth_ = nullptr;
    // The console's runtime hands the title its frames as tracked, and the
    // title smooths them itself (NuiTransformSmooth, part of the NUI library
    // it links, done natively in nui_smooth_hooks.cpp); smoothing here as
    // well would add a frame of latency on top. SFR_KINECT_SMOOTH=1
    // (kinect_host_smoothing) smooths here instead, for sfr_kinect_probe
    // --smooth; the game then leaves its own out.
    bool smooth_frames_ = kinect_host_smoothing();
    std::string depth_failure_;  // set while starting, before the worker runs
    NuiCameraElevationGetAngle elevation_get_ = nullptr;
    NuiCameraElevationSetAngle elevation_set_ = nullptr;
    NuiImageStreamOpen image_open_ = nullptr;
    NuiImageStreamGetNextFrame image_next_ = nullptr;
    NuiImageStreamReleaseFrame image_release_ = nullptr;
    std::array<ImageStream, 2> streams_{{{0, 1, 320, 240, 2}, {1, 2, 640, 480, 4}}};
    std::array<KinectImage, 2> images_{};
    std::array<std::atomic<uint32_t>, 2> image_frames_{};
    std::vector<uint8_t> scratch_;
    // The newest depth image's quality, under lock_ (record()).
    float depth_invalid_ = -1;
    uint64_t depth_quality_frame_ = 0;
    int64_t depth_quality_us_ = 0;
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

std::unique_ptr<KinectSensor> KinectSensor::open_model(int model, std::string* why) {
    if (model == 1) {
        if (auto sensor = std::make_unique<WindowsKinect>(); sensor->start(why)) return sensor;
        return nullptr;
    }
    if (model == 2) return open_kinect_v2(why);
    if (why) *why = "unknown-model";
    return nullptr;
}

}
