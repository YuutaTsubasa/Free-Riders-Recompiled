#include "kinect_sensor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <thread>

// Kinect v2 through the Kinect for Windows SDK 2.0's runtime (Kinect20.dll),
// loaded when asked for, like v1's. Its API is COM: the interfaces below are
// Kinect.h's, declared up to the last method this uses, in the SDK's order --
// a vtable is only its order, so every method before one that is called is
// listed even when unused.

namespace sfr {
namespace {

using WaitableHandle = INT_PTR;
struct CameraSpacePoint { float x, y, z; };
struct Vector4 { float x, y, z, w; };
struct Joint {
    int32_t type;  // JointType
    CameraSpacePoint position;
    int32_t state;  // TrackingState: 0 not tracked, 1 inferred, 2 tracked
};
static_assert(sizeof(Joint) == 20, "Joint layout");
constexpr UINT body_count = 6;  // BODY_COUNT

struct IBody;
struct IBodyFrame;
struct IBodyFrameReader;
struct IBodyFrameSource;

struct IKinectSensor : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeIsAvailableChanged(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeIsAvailableChanged(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsAvailableChangedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Open() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsOpen(BOOLEAN*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsAvailable(BOOLEAN*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ColorFrameSource(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DepthFrameSource(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BodyFrameSource(IBodyFrameSource**) = 0;
};

struct IBodyFrameSource : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameCaptured(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameCaptured(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameCapturedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsActive(BOOLEAN*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BodyCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenReader(IBodyFrameReader**) = 0;
};

struct IBodyFrameReader : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameArrived(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameArrived(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameArrivedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE AcquireLatestFrame(IBodyFrame**) = 0;
};

struct IBodyFrame : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAndRefreshBodyData(UINT capacity, IBody** bodies) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FloorClipPlane(Vector4*) = 0;
};

struct IBody : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetJoints(UINT capacity, Joint* joints) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetJointOrientations(UINT capacity, void* orientations) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Engaged(int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetExpressionDetectionResults(UINT, int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetActivityDetectionResults(UINT, int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppearanceDetectionResults(UINT, int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HandLeftState(int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HandLeftConfidence(int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HandRightState(int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HandRightConfidence(int32_t*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ClippedEdges(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackingId(UINT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsTracked(BOOLEAN*) = 0;
};

using GetDefaultKinectSensor = HRESULT(WINAPI*)(IKinectSensor**);

template<class T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

std::string hresult(HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(value));
    return text;
}

class WindowsKinectV2 final : public KinectSensor {
public:
    ~WindowsKinectV2() override {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        release(reader_);
        if (sensor_) sensor_->Close();
        release(sensor_);
        if (library_) FreeLibrary(library_);
    }

    const char* model() const override { return "Kinect v2"; }

    bool start(std::string* why) {
        library_ = LoadLibraryW(L"Kinect20.dll");
        if (!library_) { if (why) *why = "no-runtime"; return false; }
        const auto get_default = reinterpret_cast<GetDefaultKinectSensor>(
            reinterpret_cast<void*>(GetProcAddress(library_, "GetDefaultKinectSensor")));
        if (!get_default) { if (why) *why = "incomplete-runtime"; return false; }
        if (const HRESULT result = get_default(&sensor_); FAILED(result) || !sensor_) {
            if (why) *why = "initialize-" + hresult(result);
            return false;
        }
        if (const HRESULT result = sensor_->Open(); FAILED(result)) {
            if (why) *why = "initialize-" + hresult(result);
            return false;
        }
        // The default sensor exists and opens whether or not one is plugged
        // in; it becomes available about a second after a real one starts.
        BOOLEAN available = FALSE;
        for (int wait = 0; wait < 30 && !available; ++wait) {
            if (FAILED(sensor_->get_IsAvailable(&available))) available = FALSE;
            if (!available) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!available) { if (why) *why = "no-sensor"; return false; }
        IBodyFrameSource* source = nullptr;
        HRESULT result = sensor_->get_BodyFrameSource(&source);
        if (SUCCEEDED(result) && source) result = source->OpenReader(&reader_);
        release(source);
        if (FAILED(result) || !reader_) {
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
        KinectFrame frame;
        std::array<KinectV2Joint, kinect_v2_joint_count> mapped;
        uint64_t frames = 0, tracked = 0;
        auto reported = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            IBodyFrame* body_frame = nullptr;
            // E_PENDING until the next frame (30 a second).
            if (FAILED(reader_->AcquireLatestFrame(&body_frame)) || !body_frame) {
                release(body_frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            IBody* bodies[body_count] = {};
            const bool read = SUCCEEDED(body_frame->GetAndRefreshBodyData(body_count, bodies));
            Vector4 floor{};
            if (FAILED(body_frame->get_FloorClipPlane(&floor))) floor = {};
            release(body_frame);
            if (!read) continue;
            frame.floor_plane = {floor.x, floor.y, floor.z, floor.w};
            // v2 gives no gravity of its own; the floor's normal is up.
            frame.gravity = {floor.x, floor.y, floor.z};
            frame.bodies.clear();
            for (IBody*& body : bodies) {
                BOOLEAN is_tracked = FALSE;
                UINT64 id = 0;
                Joint joints[kinect_v2_joint_count] = {};
                if (body && SUCCEEDED(body->get_IsTracked(&is_tracked)) && is_tracked &&
                    SUCCEEDED(body->get_TrackingId(&id)) &&
                    SUCCEEDED(body->GetJoints(kinect_v2_joint_count, joints))) {
                    for (uint32_t j = 0; j < kinect_v2_joint_count; ++j)
                        mapped[j] = {{joints[j].position.x, joints[j].position.y, joints[j].position.z},
                                     uint32_t(joints[j].state < 0 ? 0 : joints[j].state)};
                    // The player slots key on 32 bits; v2's ids count up
                    // from a large base, so the low half stays distinct.
                    const uint32_t short_id = uint32_t(id) ? uint32_t(id) : uint32_t(id >> 32) | 1u;
                    kinect_v2_body(mapped, short_id, frame.bodies.emplace_back());
                }
                release(body);
            }
            ++frames;
            if (!frame.bodies.empty()) ++tracked;
            {
                std::lock_guard guard(lock_);
                frame.number = latest_.number + 1;
                latest_ = frame;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - reported >= std::chrono::seconds(5)) {
                std::cerr << "NATIVE_KINECT model=v2 frames=" << frames << " with_body=" << tracked
                          << " bodies=" << frame.bodies.size() << '\n';
                reported = now;
                frames = tracked = 0;
            }
        }
    }

    HMODULE library_ = nullptr;
    IKinectSensor* sensor_ = nullptr;
    IBodyFrameReader* reader_ = nullptr;
    std::mutex lock_;
    KinectFrame latest_;
    uint64_t taken_ = 0;
    std::jthread worker_;
};

}

std::unique_ptr<KinectSensor> open_kinect_v2(std::string* why) {
    auto sensor = std::make_unique<WindowsKinectV2>();
    if (!sensor->start(why)) return nullptr;
    return sensor;
}

}
