#include "kinect_sensor.h"
#include "kinect_smoothing.h"
#include "kinect_v2_image.h"
#include "kinect_v2_depth.h"
#include "kinect_v2_resynth.h"
#include "kinect_record.h"
#include "kinect_record_raw.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <unknwn.h>
// CoTaskMemFree, for the coordinate mapper's table (a raw recording's rays).
#pragma comment(lib, "ole32.lib")

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
struct IColorFrameSource;
struct IDepthFrameSource;
struct IBodyIndexFrameSource;
struct ICoordinateMapper;

// The cameras, for the preview window only (image()): Kinect.h's
// ColorImageFormat.
enum ColorImageFormat : int32_t {
    ColorImageFormat_None = 0, ColorImageFormat_Rgba = 1, ColorImageFormat_Yuv = 2,
    ColorImageFormat_Bgra = 3, ColorImageFormat_Bayer = 4, ColorImageFormat_Yuy2 = 5,
};

// Kinect.h's IKinectSensor, in its order: the three IsAvailableChanged event
// methods, Open, Close, get_IsOpen, get_IsAvailable, then the sources
// (colour, depth, body, body index, infrared, long-exposure infrared,
// audio), OpenMultiSourceFrameReader and get_CoordinateMapper (then
// get_UniqueKinectId and get_KinectCapabilities, unused). The slot numbers
// (IUnknown's QueryInterface, AddRef and Release are 0..2) were checked
// against IKinectSensorVtbl in the SDK 2.0 (v2.0_1409) Kinect.h.
struct IKinectSensor : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeIsAvailableChanged(WaitableHandle*) = 0;        // 3
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeIsAvailableChanged(WaitableHandle) = 0;       // 4
    virtual HRESULT STDMETHODCALLTYPE GetIsAvailableChangedEventData(WaitableHandle, IUnknown**) = 0;  // 5
    virtual HRESULT STDMETHODCALLTYPE Open() = 0;                                             // 6
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;                                            // 7
    virtual HRESULT STDMETHODCALLTYPE get_IsOpen(BOOLEAN*) = 0;                               // 8
    virtual HRESULT STDMETHODCALLTYPE get_IsAvailable(BOOLEAN*) = 0;                          // 9
    virtual HRESULT STDMETHODCALLTYPE get_ColorFrameSource(IColorFrameSource**) = 0;          // 10
    virtual HRESULT STDMETHODCALLTYPE get_DepthFrameSource(IDepthFrameSource**) = 0;          // 11
    virtual HRESULT STDMETHODCALLTYPE get_BodyFrameSource(IBodyFrameSource**) = 0;            // 12
    virtual HRESULT STDMETHODCALLTYPE get_BodyIndexFrameSource(IBodyIndexFrameSource**) = 0;  // 13
    virtual HRESULT STDMETHODCALLTYPE get_InfraredFrameSource(IUnknown**) = 0;                // 14
    virtual HRESULT STDMETHODCALLTYPE get_LongExposureInfraredFrameSource(IUnknown**) = 0;    // 15
    virtual HRESULT STDMETHODCALLTYPE get_AudioSource(IUnknown**) = 0;                        // 16
    virtual HRESULT STDMETHODCALLTYPE OpenMultiSourceFrameReader(DWORD, IUnknown**) = 0;      // 17
    virtual HRESULT STDMETHODCALLTYPE get_CoordinateMapper(ICoordinateMapper**) = 0;          // 18
};

// Kinect.h's DepthSpacePoint and ColorSpacePoint: a pixel position in the
// depth (512x424) or colour (1920x1080) image, -infinity for a point the
// mapper cannot place.
struct DepthSpacePoint { float x, y; };
// Kinect.h's CameraIntrinsics.
struct CameraIntrinsics {
    float focal_x, focal_y, centre_x, centre_y, radial2, radial4, radial6;
};
static_assert(sizeof(CameraIntrinsics) == 28, "CameraIntrinsics layout");
struct ColorSpacePoint { float x, y; };
static_assert(sizeof(CameraSpacePoint) == 12 && sizeof(DepthSpacePoint) == 8 && sizeof(ColorSpacePoint) == 8,
              "space point layouts");

// Kinect.h's ICoordinateMapper (IID 8784DF2D-16B0-481C-A11E-55E70BF25018),
// in ICoordinateMapperVtbl's order up to the last method called,
// GetDepthCameraIntrinsics (19). Slots 12..19 were checked against
// ICoordinateMapperVtbl in the SDK 2.0 (v2.0_1409) Kinect.h like the rest:
// MapDepthPointsToCameraSpace, MapDepthPointsToColorSpace,
// MapDepthFrameToCameraSpace, MapDepthFrameToColorSpace,
// MapColorFrameToDepthSpace, MapColorFrameToCameraSpace,
// GetDepthFrameToCameraSpaceTable, GetDepthCameraIntrinsics.
// Only the array forms are called, with one point: the single-point forms
// take CameraSpacePoint by value, and an array of one is the same answer
// without depending on how a 12-byte struct is passed. The preview's
// skeleton overlay uses it (image_point); without it, or for a point it
// cannot place, the typical lens of a v2 (kinect_v2_*_estimate).
struct ICoordinateMapper : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeCoordinateMappingChanged(WaitableHandle*) = 0;          // 3
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeCoordinateMappingChanged(WaitableHandle) = 0;         // 4
    virtual HRESULT STDMETHODCALLTYPE GetCoordinateMappingChangedEventData(WaitableHandle, IUnknown**) = 0;  // 5
    virtual HRESULT STDMETHODCALLTYPE MapCameraPointToDepthSpace(CameraSpacePoint, DepthSpacePoint*) = 0;    // 6
    virtual HRESULT STDMETHODCALLTYPE MapCameraPointToColorSpace(CameraSpacePoint, ColorSpacePoint*) = 0;    // 7
    virtual HRESULT STDMETHODCALLTYPE MapDepthPointToCameraSpace(DepthSpacePoint, UINT16, CameraSpacePoint*) = 0;  // 8
    virtual HRESULT STDMETHODCALLTYPE MapDepthPointToColorSpace(DepthSpacePoint, UINT16, ColorSpacePoint*) = 0;    // 9
    virtual HRESULT STDMETHODCALLTYPE MapCameraPointsToDepthSpace(UINT, const CameraSpacePoint*, UINT,
                                                                  DepthSpacePoint*) = 0;  // 10
    virtual HRESULT STDMETHODCALLTYPE MapCameraPointsToColorSpace(UINT, const CameraSpacePoint*, UINT,
                                                                  ColorSpacePoint*) = 0;  // 11
    virtual HRESULT STDMETHODCALLTYPE MapDepthPointsToCameraSpace(UINT, const DepthSpacePoint*, UINT, const UINT16*,
                                                                  UINT, CameraSpacePoint*) = 0;  // 12
    virtual HRESULT STDMETHODCALLTYPE MapDepthPointsToColorSpace(UINT, const DepthSpacePoint*, UINT, const UINT16*,
                                                                 UINT, ColorSpacePoint*) = 0;  // 13
    // Every pixel of a depth frame (512x424 millimetres) into camera space;
    // -infinity for a pixel without a depth. the depth made for the game's
    // reprojection (kinect_v2_depth.h).
    virtual HRESULT STDMETHODCALLTYPE MapDepthFrameToCameraSpace(UINT, const UINT16*, UINT, CameraSpacePoint*) = 0;  // 14
    virtual HRESULT STDMETHODCALLTYPE MapDepthFrameToColorSpace(UINT, const UINT16*, UINT, ColorSpacePoint*) = 0;    // 15
    virtual HRESULT STDMETHODCALLTYPE MapColorFrameToDepthSpace(UINT, const UINT16*, UINT, DepthSpacePoint*) = 0;    // 16
    virtual HRESULT STDMETHODCALLTYPE MapColorFrameToCameraSpace(UINT, const UINT16*, UINT, CameraSpacePoint*) = 0;  // 17
    // A ray (PointF: x / z, y / z) per depth pixel, the caller's to free
    // with CoTaskMemFree; for a raw recording's header (kinect_record_raw.h).
    virtual HRESULT STDMETHODCALLTYPE GetDepthFrameToCameraSpaceTable(UINT32*, void**) = 0;                          // 18
    // The depth camera's lens (focal lengths and centre in 512x424 pixels,
    // radial distortion), for the fit view's stretch; zero until the
    // sensor has sent its calibration.
    virtual HRESULT STDMETHODCALLTYPE GetDepthCameraIntrinsics(CameraIntrinsics*) = 0;                               // 19
};

struct IColorFrame;
struct IColorFrameReader;
struct IDepthFrame;
struct IDepthFrameReader;
struct IBodyIndexFrame;
struct IBodyIndexFrameReader;

// IColorFrameSource: FrameCaptured events, get_IsActive, OpenReader (then
// CreateFrameDescription, get_FrameDescription, get_KinectSensor, unused).
struct IColorFrameSource : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameCaptured(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameCaptured(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameCapturedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsActive(BOOLEAN*) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenReader(IColorFrameReader**) = 0;
};

// Every frame reader, as the body's: FrameArrived events, then
// AcquireLatestFrame (then get_IsPaused, put_IsPaused and the source, unused).
struct IColorFrameReader : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameArrived(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameArrived(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameArrivedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE AcquireLatestFrame(IColorFrame**) = 0;
};

// IColorFrame: get_RawColorImageFormat, get_FrameDescription,
// CopyRawFrameDataToArray, AccessRawUnderlyingBuffer,
// CopyConvertedFrameDataToArray (then CreateFrameDescription,
// get_ColorCameraSettings, get_RelativeTime, get_ColorFrameSource, unused).
struct IColorFrame : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_RawColorImageFormat(ColorImageFormat*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FrameDescription(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyRawFrameDataToArray(UINT capacity, BYTE* data) = 0;
    virtual HRESULT STDMETHODCALLTYPE AccessRawUnderlyingBuffer(UINT* capacity, BYTE** buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyConvertedFrameDataToArray(UINT capacity, BYTE* data, ColorImageFormat) = 0;
};

// IDepthFrameSource: FrameCaptured events, get_IsActive, OpenReader (then
// the reliable distances, get_FrameDescription, get_KinectSensor, unused).
struct IDepthFrameSource : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameCaptured(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameCaptured(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameCapturedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsActive(BOOLEAN*) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenReader(IDepthFrameReader**) = 0;
};

struct IDepthFrameReader : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameArrived(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameArrived(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameArrivedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE AcquireLatestFrame(IDepthFrame**) = 0;
};

// IDepthFrame: CopyFrameDataToArray (3), AccessUnderlyingBuffer (4),
// get_FrameDescription (5), get_RelativeTime (6; then the source and the
// reliable distances, unused), as IDepthFrameVtbl in Kinect.h. Millimetres,
// 0 for nothing measured.
struct IDepthFrame : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CopyFrameDataToArray(UINT capacity, UINT16* data) = 0;
    virtual HRESULT STDMETHODCALLTYPE AccessUnderlyingBuffer(UINT* capacity, UINT16** buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FrameDescription(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RelativeTime(INT64*) = 0;  // TIMESPAN, 100 ns units
};

// IBodyIndexFrameSource: FrameCaptured events, get_IsActive, OpenReader
// (then get_FrameDescription, get_KinectSensor, unused).
struct IBodyIndexFrameSource : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameCaptured(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameCaptured(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameCapturedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsActive(BOOLEAN*) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenReader(IBodyIndexFrameReader**) = 0;
};

struct IBodyIndexFrameReader : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SubscribeFrameArrived(WaitableHandle*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnsubscribeFrameArrived(WaitableHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameArrivedEventData(WaitableHandle, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE AcquireLatestFrame(IBodyIndexFrame**) = 0;
};

// IBodyIndexFrame: CopyFrameDataToArray (3), AccessUnderlyingBuffer (4),
// get_FrameDescription (5), get_RelativeTime (6; then the source, unused),
// as IBodyIndexFrameVtbl in Kinect.h. Each depth pixel's body (0..5, its
// index in GetAndRefreshBodyData's array), 255 for none.
struct IBodyIndexFrame : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CopyFrameDataToArray(UINT capacity, BYTE* data) = 0;
    virtual HRESULT STDMETHODCALLTYPE AccessUnderlyingBuffer(UINT* capacity, BYTE** buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FrameDescription(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RelativeTime(INT64*) = 0;
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

// IBodyFrame: GetAndRefreshBodyData (3), get_FloorClipPlane (4),
// get_RelativeTime (5; then get_BodyFrameSource, unused).
struct IBodyFrame : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAndRefreshBodyData(UINT capacity, IBody** bodies) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FloorClipPlane(Vector4*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RelativeTime(INT64*) = 0;  // TIMESPAN, 100 ns units
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
        {
            std::lock_guard guard(image_lock_);
            close_images();
        }
        release(game_depth_reader_);
        release(game_index_reader_);
        {
            std::lock_guard guard(mapper_lock_);
            release(mapper_);
        }
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
        std::cerr << "NATIVE_KINECT model=v2 smooth=" << (smooth_ ? 1 : 0) << '\n';
        open_game_depth();  // failing to open leaves the skeleton path (game_depth_ false)
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        return true;
    }

    // the Kinect v2 depth (kinect_v2_depth.h): a v1's depth stream for the
    // game, made on the sensor's own thread.
    bool has_depth_stream() const override { return game_depth_; }
    std::string depth_failure() const override { return game_depth_ ? std::string() : depth_failure_; }
    bool game_image(KinectImageKind kind, KinectImage& out) override {
        if (kind != KinectImageKind::depth_and_player || !game_depth_) return false;
        std::lock_guard guard(game_image_lock_);
        if (game_image_.number == 0 || game_image_.number == out.number) return false;
        out = game_image_;
        return true;
    }

    bool next(KinectFrame& frame) override {
        std::lock_guard guard(lock_);
        if (latest_.number == taken_) return false;
        frame = latest_;
        taken_ = latest_.number;
        return true;
    }

    // The cameras, for the preview window (kinect_v2_image.h). Opened on
    // the first call rather than with the sensor, so the game, which never
    // asks (has_depth_stream stays false), reads skeletons alone; read on
    // the caller's thread, so their cost is the preview's.
    bool image(KinectImageKind kind, KinectImage& out) override {
        std::lock_guard guard(image_lock_);
        const auto now = std::chrono::steady_clock::now();
        image_asked_ = now;
        if (!images_open_ && now >= images_retry_at_) open_images(now);
        if (kind == KinectImageKind::colour) read_colour();
        else read_depth();
        const KinectImage& latest = images_[size_t(kind)];
        if (latest.number == 0 || latest.number == out.number) return false;
        out = latest;
        return true;
    }

    void set_preview_common_view(bool common) override {
        preview_common_.store(common, std::memory_order_relaxed);
    }

    // The preview's view and the launcher's tuning: taken up on the sensor's
    // thread (apply_depth_settings) before its next depth image.
    void set_depth_settings(const KinectDepthSettings& settings) override {
        std::lock_guard guard(depth_settings_lock_);
        pending_depth_settings_ = settings;
    }

    bool ideal_snapshot(KinectIdealSnapshot& snapshot) override {
        if (!game_depth_) return false;
        std::lock_guard guard(snapshot_lock_);
        snapshot = snapshot_;
        return true;
    }

    bool picture_rect(KinectImageKind kind, KinectImageRect& rect) override {
        rect = kinect_v2_picture_rect(kind, preview_view());
        return true;
    }

    // The preview's sizes, through the sensor's own coordinate mapper (its
    // factory calibration of both cameras), or a typical v2's lens without it
    // or for a point it cannot place (kinect_v2_place).
    bool image_point(KinectImageKind kind, const std::array<float, 3>& given, KinectImagePoint& point) override {
        float mapped[2] = {};
        bool answered = false;
        // The frames handed out are in the virtual v1's place in
        // made-depth mode; the sensor's own images are not.
        std::array<float, 3> p = given;
        if (game_depth_) {
            std::lock_guard guard(lock_);
            p = kinect_depth_view_unpoint(published_view_, given);
        }
        if (ICoordinateMapper* mapper = coordinate_mapper()) {
            std::lock_guard mapping(map_lock_);
            const CameraSpacePoint camera{p[0], p[1], p[2]};
            HRESULT result = E_FAIL;
            if (kind == KinectImageKind::colour) {
                ColorSpacePoint out{-INFINITY, -INFINITY};
                result = mapper->MapCameraPointsToColorSpace(1, &camera, 1, &out);
                mapped[0] = out.x;
                mapped[1] = out.y;
            } else {
                DepthSpacePoint out{-INFINITY, -INFINITY};
                result = mapper->MapCameraPointsToDepthSpace(1, &camera, 1, &out);
                mapped[0] = out.x;
                mapped[1] = out.y;
            }
            answered = SUCCEEDED(result);
        }
        const bool placed = answered && std::isfinite(mapped[0]) && std::isfinite(mapped[1]);
        mapped_points_[placed ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
        point = kinect_v2_place(kind, p, answered ? mapped : nullptr, preview_view());
        return true;
    }

    // Sensor-space points into the preview's colour image, all in one call
    // to the mapper (the points that are points only), the typical lens for
    // what it cannot place (kinect_v2_place), as image_point places one.
    bool colour_image_positions(const std::vector<float>& points, std::vector<float>& positions) override {
        const size_t count = points.size() / 3;
        positions.assign(count * 2, NAN);
        std::vector<CameraSpacePoint> given;
        std::vector<size_t> index;
        given.reserve(count);
        index.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const float x = points[3 * i], y = points[3 * i + 1], z = points[3 * i + 2];
            if (!(z > 0.0f) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
            given.push_back({x, y, z});
            index.push_back(i);
        }
        if (given.empty()) return true;
        std::vector<ColorSpacePoint> mapped(given.size(), ColorSpacePoint{-INFINITY, -INFINITY});
        bool answered = false;
        if (ICoordinateMapper* mapper = coordinate_mapper()) {
            std::lock_guard mapping(map_lock_);
            answered = SUCCEEDED(mapper->MapCameraPointsToColorSpace(UINT(given.size()), given.data(), UINT(mapped.size()),
                                                                     mapped.data()));
        }
        const KinectV2View view = preview_view();
        for (size_t j = 0; j < given.size(); ++j) {
            const float at[2] = {mapped[j].x, mapped[j].y};
            const KinectImagePoint point =
                kinect_v2_place(KinectImageKind::colour, {given[j].x, given[j].y, given[j].z}, answered ? at : nullptr, view);
            if (!point.visible) continue;
            positions[2 * index[j]] = point.x;
            positions[2 * index[j] + 1] = point.y;
        }
        return true;
    }

private:
    KinectV2View preview_view() const {
        return preview_common_.load(std::memory_order_relaxed) ? KinectV2View::common : KinectV2View::original;
    }

    // The sensor's coordinate mapper, asked for on the preview's first point
    // (the game never asks): null when the runtime would not give one.
    ICoordinateMapper* coordinate_mapper() {
        std::lock_guard guard(mapper_lock_);
        if (!mapper_asked_) {
            mapper_asked_ = true;
            const HRESULT result = sensor_->get_CoordinateMapper(&mapper_);
            if (FAILED(result)) mapper_ = nullptr;
            std::cerr << "NATIVE_KINECT_MAPPER model=v2 result=" << hresult(result) << (mapper_ ? "" : " (lens estimate)")
                      << std::endl;
        }
        return mapper_;
    }

    // Under image_lock_ from here to read_depth.
    void open_images(std::chrono::steady_clock::time_point now) {
        IColorFrameSource* colour = nullptr;
        HRESULT colour_result = sensor_->get_ColorFrameSource(&colour);
        if (SUCCEEDED(colour_result) && colour) colour_result = colour->OpenReader(&colour_reader_);
        release(colour);
        IDepthFrameSource* depth = nullptr;
        HRESULT depth_result = sensor_->get_DepthFrameSource(&depth);
        if (SUCCEEDED(depth_result) && depth) depth_result = depth->OpenReader(&depth_reader_);
        release(depth);
        IBodyIndexFrameSource* index = nullptr;
        HRESULT index_result = sensor_->get_BodyIndexFrameSource(&index);
        if (SUCCEEDED(index_result) && index) index_result = index->OpenReader(&index_reader_);
        release(index);
        images_open_ = colour_reader_ || depth_reader_;
        // Nothing opened: try again in a while rather than every call.
        if (!images_open_) images_retry_at_ = now + std::chrono::seconds(5);
        std::cerr << "NATIVE_KINECT_IMAGE model=v2 open colour=" << hresult(colour_result) << " depth="
                  << hresult(depth_result) << " body_index=" << hresult(index_result) << std::endl;
    }

    void close_images() {
        if (!images_open_ && !index_reader_) return;
        release(colour_reader_);
        release(depth_reader_);
        release(index_reader_);
        images_open_ = false;
        has_body_index_ = false;
        std::cerr << "NATIVE_KINECT_IMAGE model=v2 closed" << std::endl;
    }

    void read_colour() {
        if (!colour_reader_) return;
        IColorFrame* frame = nullptr;
        // E_PENDING (or no frame) until a new one arrives: nothing new.
        if (FAILED(colour_reader_->AcquireLatestFrame(&frame)) || !frame) { release(frame); return; }
        constexpr UINT pixels = kinect_v2_colour_width * kinect_v2_colour_height;
        ColorImageFormat format = ColorImageFormat_None;
        if (FAILED(frame->get_RawColorImageFormat(&format))) format = ColorImageFormat_None;
        HRESULT result = E_FAIL;
        KinectImage& made = images_[size_t(KinectImageKind::colour)];
        const KinectV2View view = preview_view();
        if (format == ColorImageFormat_Yuy2) {
            // The camera's own format: copied as it is and converted only
            // where the preview's smaller image samples it.
            colour_scratch_.resize(size_t(pixels) * 2);
            result = frame->CopyRawFrameDataToArray(UINT(colour_scratch_.size()), colour_scratch_.data());
            if (SUCCEEDED(result))
                kinect_v2_colour_from_yuy2(colour_scratch_.data(), kinect_v2_colour_width, kinect_v2_colour_height, view, made);
        }
        if (FAILED(result)) {
            colour_scratch_.resize(size_t(pixels) * 4);
            result = frame->CopyConvertedFrameDataToArray(UINT(colour_scratch_.size()), colour_scratch_.data(),
                                                          ColorImageFormat_Bgra);
            if (SUCCEEDED(result))
                kinect_v2_colour_from_bgra(colour_scratch_.data(), kinect_v2_colour_width, kinect_v2_colour_height, view, made);
        }
        release(frame);
        if (SUCCEEDED(result)) {
            ++made.number;
            image_frames_[size_t(KinectImageKind::colour)].fetch_add(1, std::memory_order_relaxed);
        } else if (!colour_failure_logged_) {
            colour_failure_logged_ = true;
            std::cerr << "NATIVE_KINECT_IMAGE model=v2 colour_copy=" << hresult(result) << " format=" << int(format)
                      << std::endl;
        }
    }

    void read_depth() {
        constexpr UINT pixels = kinect_v2_depth_width * kinect_v2_depth_height;
        if (index_reader_) {
            // Which body each pixel is, for the preview's tint; its own
            // reader, so it may be a frame apart from the depth.
            IBodyIndexFrame* frame = nullptr;
            if (SUCCEEDED(index_reader_->AcquireLatestFrame(&frame)) && frame) {
                body_index_.resize(pixels);
                has_body_index_ = SUCCEEDED(frame->CopyFrameDataToArray(pixels, body_index_.data()));
            }
            release(frame);
        }
        if (!depth_reader_) return;
        IDepthFrame* frame = nullptr;
        if (FAILED(depth_reader_->AcquireLatestFrame(&frame)) || !frame) { release(frame); return; }
        depth_scratch_.resize(pixels);
        const HRESULT result = frame->CopyFrameDataToArray(pixels, depth_scratch_.data());
        release(frame);
        if (FAILED(result)) {
            if (!depth_failure_logged_) {
                depth_failure_logged_ = true;
                std::cerr << "NATIVE_KINECT_IMAGE model=v2 depth_copy=" << hresult(result) << std::endl;
            }
            return;
        }
        {
            // How much of the image has no depth, for a recording
            // (kinect_record.h): another sensor's light shows there first.
            std::lock_guard quality(depth_quality_lock_);
            depth_invalid_ = kinect_depth_invalid_ratio(depth_scratch_.data(), pixels, 0);
            ++depth_quality_frame_;
            depth_quality_us_ = kinect_record_now_us();
        }
        KinectImage& made = images_[size_t(KinectImageKind::depth_and_player)];
        kinect_v2_depth_image(depth_scratch_.data(), has_body_index_ ? body_index_.data() : nullptr,
                              kinect_v2_depth_width, kinect_v2_depth_height, preview_view(), made);
        ++made.number;
        image_frames_[size_t(KinectImageKind::depth_and_player)].fetch_add(1, std::memory_order_relaxed);
    }

    // The newest depth image's share of pixels without a depth, into a
    // recorded frame; none while the depth reader is closed (the game never
    // opens it; the probe does while recording).
    void depth_quality(KinectRecordFrame& record) {
        std::lock_guard quality(depth_quality_lock_);
        if (!depth_quality_frame_ || kinect_record_now_us() - depth_quality_us_ > 1000000) return;
        record.depth_invalid = depth_invalid_;
        record.depth_frame = depth_quality_frame_;
        record.depth_steady_us = depth_quality_us_;
    }

    // The cameras close again once the preview has not asked for a while
    // (its window closed), from the sensor's own thread; never waits for a
    // preview busy converting a frame.
    void close_idle_images() {
        std::unique_lock guard(image_lock_, std::try_to_lock);
        if (!guard.owns_lock() || !images_open_) return;
        if (std::chrono::steady_clock::now() - image_asked_ > std::chrono::seconds(3)) close_images();
    }

    // The game's own depth and body index readers, apart from the preview's
    // (every reader gets every frame), so neither takes the other's frames.
    void open_game_depth() {
        IDepthFrameSource* depth = nullptr;
        HRESULT depth_result = sensor_->get_DepthFrameSource(&depth);
        if (SUCCEEDED(depth_result) && depth) depth_result = depth->OpenReader(&game_depth_reader_);
        release(depth);
        IBodyIndexFrameSource* index = nullptr;
        HRESULT index_result = sensor_->get_BodyIndexFrameSource(&index);
        if (SUCCEEDED(index_result) && index) index_result = index->OpenReader(&game_index_reader_);
        release(index);
        // Without the body index there are no players in the image, which is
        // what the game's depth view looks for: both or neither.
        game_depth_ = game_depth_reader_ && game_index_reader_;
        if (!game_depth_) {
            release(game_depth_reader_);
            release(game_index_reader_);
            depth_failure_ = FAILED(depth_result) ? "depth-" + hresult(depth_result)
                                                  : "body-index-" + hresult(index_result);
        }
        std::ostringstream line;
        const KinectDepthSettings& settings = viewer_.settings();
        line << "NATIVE_KINECT_V2_DEPTH " << (game_depth_ ? "on" : "off") << " view=" << settings.view_name();
        if (settings.mode == KinectDepthViewMode::fit) {
            if (settings.fit_scale > 0) {
                fit_scale_ = settings.fit_scale;
                fit_source_ = "setting";
            } else {
                fit_lens_pending_ = !fit_from_sensor();
            }
            viewer_.set_fit_scale(fit_scale_);
            line << fit_description();
        } else if (settings.mode == KinectDepthViewMode::player) {
            line << " distance=" << settings.distance << " height=" << settings.height;
        } else if (settings.mode == KinectDepthViewMode::ideal) {
            // No stretch: the v1's 0.8..4 m is measured from the virtual
            // camera, which stands distance in front of the players.
            char text[200];
            std::snprintf(text, sizeof text,
                          " distance=%.2f height=%.2f follow=%.2f deadband=%.2f true_range=0.80-4.00m from the virtual camera"
                          " (the players at %.2fm)",
                          double(settings.distance), double(settings.height), double(settings.follow),
                          double(settings.deadband), double(settings.distance));
            line << text;
        }
        // The skeleton's own transform: in real metres (1.0) unless
        // SFR_KINECT_V2_SKELETON_STRETCH says otherwise ("depth": the
        // depth's k, which may still change once the sensor gives its lens).
        if (settings.mode == KinectDepthViewMode::fit) {
            char text[96];
            if (settings.skeleton_stretch > 0)
                std::snprintf(text, sizeof text, " skeleton_stretch=%.3f%s", double(settings.skeleton_stretch),
                              settings.skeleton_stretch == 1.0f ? " (real metres)" : "");
            else
                std::snprintf(text, sizeof text, " skeleton_stretch=depth's (%.3f)", double(fit_scale_));
            line << text;
        }
        if (settings.mode == KinectDepthViewMode::ideal)
            line << " skeleton_distance="
                 << (settings.skeleton_distance > 0 ? std::to_string(settings.skeleton_distance) : std::string("depth's"));
        line << " depth=" << hresult(depth_result) << " body_index=" << hresult(index_result);
        std::cerr << line.str() << std::endl;
    }

    // The fit view's stretch from the sensor's own depth lens; false while
    // it has not sent its calibration (the typical lens meanwhile).
    bool fit_from_sensor() {
        ICoordinateMapper* mapper = coordinate_mapper();
        if (!mapper) return false;
        CameraIntrinsics lens{};
        std::lock_guard mapping(map_lock_);
        if (FAILED(mapper->GetDepthCameraIntrinsics(&lens)) || !(lens.focal_y > 100.0f && lens.focal_y < 1000.0f))
            return false;
        fit_focal_ = lens.focal_y;
        fit_scale_ = kinect_v2_fit_scale(lens.focal_y);
        fit_source_ = "sensor";
        return true;
    }

    std::string fit_description() const {
        char text[160];
        std::snprintf(text, sizeof text, " scale=%.3f focal=%.1f (%s) true_range=%.2f-%.2fm", double(fit_scale_),
                      double(fit_focal_), fit_source_, double(kinect_depth_true_minimum(fit_scale_)),
                      double(kinect_depth_true_maximum(fit_scale_)));
        return text;
    }

    // Where the virtual v1 is for this body frame of the sensor's own space
    // (KinectDepthViewer: the player view's latch follows it, the ideal view
    // keeps its bodies for the next depth image).
    KinectDepthView game_view(const KinectFrame& frame, double seconds) {
        // The sensor sends its lens a moment after it starts: asked again
        // about once a second until then.
        if (viewer_.settings().mode == KinectDepthViewMode::fit && fit_lens_pending_ && ++fit_lens_tries_ % 30 == 1 &&
            fit_from_sensor()) {
            fit_lens_pending_ = false;
            viewer_.set_fit_scale(fit_scale_);
            std::cerr << "NATIVE_KINECT_V2_DEPTH fit" << fit_description() << std::endl;
        }
        return viewer_.body_frame(frame, seconds);
    }

    // New settings from set_depth_settings, on the sensor's thread. The fit
    // view's stretch is the setting's, or the sensor's lens (asked for now
    // if it has not been). One log line when the view changes, none for
    // tuning alone (a slider being dragged sends one every frame).
    void apply_depth_settings() {
        std::optional<KinectDepthSettings> settings;
        {
            std::lock_guard guard(depth_settings_lock_);
            settings.swap(pending_depth_settings_);
        }
        if (!settings) return;
        const bool new_view = settings->mode != viewer_.settings().mode;
        viewer_.set_settings(*settings);
        if (settings->mode == KinectDepthViewMode::fit) {
            if (settings->fit_scale > 0) {
                fit_scale_ = settings->fit_scale;
                fit_source_ = "setting";
                fit_lens_pending_ = false;
            } else if (std::string_view(fit_source_) != "sensor") {
                fit_lens_pending_ = !fit_from_sensor();
            }
            viewer_.set_fit_scale(fit_scale_);
        }
        if (new_view) {
            char text[160];
            std::snprintf(text, sizeof text, " distance=%.2f height=%.2f follow=%.2f deadband=%.2f",
                          double(settings->distance), double(settings->height), double(settings->follow),
                          double(settings->deadband));
            std::cerr << "NATIVE_KINECT_V2_DEPTH set view=" << settings->view_name() << text
                      << (settings->mode == KinectDepthViewMode::fit ? fit_description() : std::string()) << std::endl;
        }
    }

    // The ideal camera and the players for the preview's diagram, after a
    // depth image is made; 1P and 2P as the title's slots would take them.
    void publish_snapshot() {
        KinectIdealSnapshot made = kinect_ideal_snapshot(viewer_.bodies(), viewer_.ideal(), viewer_.settings());
        const auto slots = snapshot_slots_.assign(viewer_.bodies());
        for (KinectIdealPlayer& player : made.players)
            for (size_t slot = 0; slot < slots.size(); ++slot)
                if (slots[slot] && slots[slot]->tracking_id == player.tracking_id) player.slot = int(slot);
        std::lock_guard guard(snapshot_lock_);
        snapshot_ = std::move(made);
    }

    // The ideal view's camera for the log: where it stands (levelled space),
    // its tilt taken out, the reference it follows, and how many bodies.
    std::string ideal_description(const KinectDepthView& view) const {
        const KinectIdealState& state = viewer_.ideal();
        float pitch = 0, roll = 0;
        kinect_floor_tilt(state.floor, pitch, roll);
        // The floor as the sensor gives it now and as smoothed, the sensor's
        // height above it (its distance), and the camera's: what decides
        // where a player lands up and down in the made image.
        const auto& given = viewer_.bodies().floor_plane;
        char text[480];
        std::snprintf(text, sizeof text,
                      " depth_camera=%.3f,%.3f,%.3f level=%d pitch=%.1f roll=%.1f floor_d=%.3f reference=%.3f,%.3f%s"
                      " reference_bodies=%u floor_given=%.3f,%.3f,%.3f,%.3f sensor_height=%s camera_height=%s height=%.2f",
                      double(view.origin[0]), double(view.origin[1]), double(view.origin[2]), state.floor_known ? 1 : 0,
                      double(pitch), double(roll), double(state.floor[3]), double(state.reference[0]),
                      double(state.reference[1]), state.placed ? "" : " (none yet)", state.bodies, double(given[0]),
                      double(given[1]), double(given[2]), double(given[3]),
                      state.height_known ? std::to_string(state.floor[3]).substr(0, 5).c_str() : "unknown",
                      state.height_known ? std::to_string(state.floor[3] + view.origin[1]).substr(0, 5).c_str()
                                         : "the sensor's",
                      double(viewer_.settings().height));
        return text;
    }

    // A raw recording (kinect_record_raw.h) opens once the sensor's mapper
    // gives points (its calibration is then in, and with it the table), or
    // after about five seconds without (the lens, or a typical one).
    void record_raw(KinectRawRecordFile& file, bool mapped, const KinectImage& made) {
        constexpr UINT pixels = kinect_v2_depth_width * kinect_v2_depth_height;
        if (!file.is_open()) {
            if (!mapped && ++raw_waited_ < 150) return;
            KinectRawHeader header;
            header.width = kinect_v2_depth_width;
            header.height = kinect_v2_depth_height;
            if (ICoordinateMapper* mapper = coordinate_mapper()) {
                std::lock_guard mapping(map_lock_);
                CameraIntrinsics lens{};
                if (SUCCEEDED(mapper->GetDepthCameraIntrinsics(&lens)) && lens.focal_y > 100.0f && lens.focal_y < 1000.0f)
                    header.lens = {lens.focal_x, lens.focal_y, lens.centre_x, lens.centre_y, lens.radial2, lens.radial4,
                                   lens.radial6};
                UINT32 count = 0;
                void* table = nullptr;
                if (SUCCEEDED(mapper->GetDepthFrameToCameraSpaceTable(&count, &table)) && table && count == pixels) {
                    const float* rays = static_cast<const float*>(table);
                    header.rays.assign(rays, rays + 2 * size_t(pixels));
                    header.rays_from = KinectRawRays::sensor_table;
                }
                if (table) CoTaskMemFree(table);
            }
            if (header.rays_from == KinectRawRays::sensor_table) {
                bool flip_x = false, flip_y = false;
                if (!kinect_raw_rays_orientation(header, flip_x, flip_y)) header.rays_from = KinectRawRays::unknown;
            }
            if (header.rays_from != KinectRawRays::sensor_table) {
                const bool lens = header.lens[1] > 0;
                kinect_v2_depth_rays_from_lens(lens ? header.lens[0] : 366.0f, lens ? header.lens[1] : 366.0f,
                                               lens ? header.lens[2] : 256.0f, lens ? header.lens[3] : 212.0f,
                                               kinect_v2_depth_width, kinect_v2_depth_height, header.rays);
                header.rays_from = lens ? KinectRawRays::sensor_lens : KinectRawRays::typical_lens;
            }
            if (!file.open(header)) return;
        }
        KinectRawFrame frame;
        frame.steady_us = made.steady_us;
        frame.depth_time = game_depth_time_;
        frame.body_time = depth_body_time_;
        frame.number = made.number;
        frame.flags = mapped ? kinect_raw_flag_mapped : 0;
        kinect_raw_set_bodies(viewer_.bodies(), frame);
        frame.depth = game_millimetres_;
        if (game_body_index_.size() == pixels) frame.body_index = game_body_index_;
        else frame.body_index.assign(pixels, kinect_v2_no_body);
        file.write(frame);
    }

    // The newest depth and body index frames, and once both are of the same
    // moment, the game's image from them through the view of the newest body
    // frame. On the sensor's thread.
    void poll_game_depth() {
        constexpr UINT pixels = kinect_v2_depth_width * kinect_v2_depth_height;
        IDepthFrame* depth = nullptr;
        if (SUCCEEDED(game_depth_reader_->AcquireLatestFrame(&depth)) && depth) {
            game_millimetres_.resize(pixels);
            INT64 time = 0;
            if (SUCCEEDED(depth->CopyFrameDataToArray(pixels, game_millimetres_.data())) &&
                SUCCEEDED(depth->get_RelativeTime(&time)))
                game_depth_time_ = time;
        }
        release(depth);
        IBodyIndexFrame* index = nullptr;
        if (SUCCEEDED(game_index_reader_->AcquireLatestFrame(&index)) && index) {
            game_body_index_.resize(pixels);
            INT64 time = 0;
            if (SUCCEEDED(index->CopyFrameDataToArray(pixels, game_body_index_.data())) &&
                SUCCEEDED(index->get_RelativeTime(&time)))
                game_index_time_ = time;
        }
        release(index);
        // Both readers' frames are the same capture (one camera); a frame
        // apart they would put the players' outline a frame off its depth.
        if (!game_depth_time_ || game_depth_time_ == game_made_time_ ||
            std::llabs(game_depth_time_ - game_index_time_) > 166666)  // half a frame, 100 ns units
            return;
        game_made_time_ = game_depth_time_;
        bool mapped = false;
        if (ICoordinateMapper* mapper = coordinate_mapper()) {
            game_points_.resize(size_t(pixels) * 3);
            std::lock_guard mapping(map_lock_);
            mapped = SUCCEEDED(mapper->MapDepthFrameToCameraSpace(
                pixels, game_millimetres_.data(), pixels, reinterpret_cast<CameraSpacePoint*>(game_points_.data())));
            // A mapper without its calibration yet answers no point at all.
            if (mapped) {
                mapped = false;
                for (size_t i = 0; i < pixels && !mapped; ++i)
                    mapped = std::isfinite(game_points_[3 * i + 2]) && game_points_[3 * i + 2] > 0;
            }
        }
        if (!mapped) kinect_v2_depth_points_from_lens(game_millimetres_.data(), kinect_v2_depth_width,
                                                      kinect_v2_depth_height, game_points_);
        (mapped ? game_mapped_frames_ : game_lens_frames_).fetch_add(1, std::memory_order_relaxed);
        // The view of the newest body frame (fit, sensor, player), or the
        // ideal view stepped now on the depth frame's own clock.
        const KinectDepthView view = viewer_.depth_frame(double(game_depth_time_) * 1e-7);
        publish_snapshot();
        if (viewer_.settings().mode == KinectDepthViewMode::ideal &&
            viewer_.ideal().floor_known != ideal_floor_logged_known_) {
            ideal_floor_logged_known_ = viewer_.ideal().floor_known;
            if (ideal_floor_logged_known_)
                std::cerr << "NATIVE_KINECT_V2_DEPTH ideal floor found: levelled" << ideal_description(view) << std::endl;
        }
        // The floor's distance (the sensor's height) comes and goes apart
        // from its normal: one line each time that changes, at most one a
        // second, so a log shows why the camera stands where it does.
        if (viewer_.settings().mode == KinectDepthViewMode::ideal && viewer_.ideal().floor_known &&
            viewer_.ideal().height_known != ideal_height_logged_known_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - ideal_height_logged_at_ >= std::chrono::seconds(1)) {
                ideal_height_logged_known_ = viewer_.ideal().height_known;
                ideal_height_logged_at_ = now;
                std::cerr << "NATIVE_KINECT_V2_DEPTH ideal "
                          << (ideal_height_logged_known_ ? "floor height found" : "floor without a height (W=0): the camera at the sensor's own height")
                          << ideal_description(view) << std::endl;
            }
        }
        if (viewer_.settings().mode == KinectDepthViewMode::ideal && !viewer_.ideal().floor_known && !ideal_no_floor_logged_) {
            ideal_no_floor_logged_ = true;
            std::cerr << "NATIVE_KINECT_V2_DEPTH ideal no floor plane yet: not levelled, the camera at the sensor's own height"
                      << std::endl;
        }
        KinectImage made;
        const KinectDepthStats stats =
            kinect_v2_synthesize_depth(game_points_.data(), game_body_index_.size() == pixels ? game_body_index_.data() : nullptr,
                                       pixels, view, made);
        // Its clocks and view, for a depth recording (kinect_record_depth.h:
        // the game records what it is handed, the probe what it reads).
        made.steady_us = kinect_record_now_us();
        made.sdk_time = game_depth_time_;
        made.view_origin = view.origin;
        made.view_z_scale = view.z_scale;
        made.view_rotated = view.rotated;
        made.view_rotation = view.rotation;
        {
            std::lock_guard guard(game_image_lock_);
            made.number = game_image_.number + 1;
            game_image_ = made;
        }
        game_frames_.fetch_add(1, std::memory_order_relaxed);
        game_valid_.store(stats.valid, std::memory_order_relaxed);
        game_players_.store(stats.players, std::memory_order_relaxed);
        // SFR_KINECT_RECORD_RAW: what this image was made from.
        if (KinectRawRecordFile* raw = kinect_record_raw_file()) record_raw(*raw, mapped, made);
    }

    void run(std::stop_token stop) {
        KinectFrame frame;
        std::array<KinectV2Joint, kinect_v2_joint_count> mapped;
        uint64_t frames = 0, tracked = 0;
        auto reported = std::chrono::steady_clock::now();
        // Per body slot: the clipped edges last written to the log and when,
        // for NATIVE_KINECT_CLIPPED (on a change, once a second at most).
        std::array<uint8_t, 6> logged_edges{};
        std::array<std::chrono::steady_clock::time_point, 6> logged_edges_at{};
        // SFR_KINECT_RECORD (kinect_record.h): every frame, raw.
        const bool recording = kinect_record_skeletons() != nullptr;
        uint64_t recorded_frames = 0;
        const auto began = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            close_idle_images();
            if (game_depth_) {
                apply_depth_settings();
                poll_game_depth();
            }
            IBodyFrame* body_frame = nullptr;
            // E_PENDING until the next frame (30 a second).
            if (FAILED(reader_->AcquireLatestFrame(&body_frame)) || !body_frame) {
                release(body_frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            IBody* bodies[body_count] = {};
            const auto captured_at = std::chrono::steady_clock::now();
            const bool read = SUCCEEDED(body_frame->GetAndRefreshBodyData(body_count, bodies));
            Vector4 floor{};
            if (FAILED(body_frame->get_FloorClipPlane(&floor))) floor = {};
            // The body frames' clock: the recordings', and the depth view's
            // (KinectDepthViewer), so a raw recording replays it.
            INT64 relative_time = 0;
            if (FAILED(body_frame->get_RelativeTime(&relative_time))) relative_time = 0;
            release(body_frame);
            if (!read) {
                for (IBody*& body : bodies) release(body);
                continue;
            }
            KinectRecordFrame record;
            if (recording) {
                record.steady_us = kinect_record_now_us();
                record.sensor = "v2";
                record.frame = ++recorded_frames;
                record.sdk_time = relative_time;
                record.floor = {floor.x, floor.y, floor.z, floor.w};
                depth_quality(record);
            }
            frame.captured_at = captured_at;
            frame.floor_plane = {floor.x, floor.y, floor.z, floor.w};
            // v2 gives no gravity of its own; the floor's normal is up.
            frame.gravity = {floor.x, floor.y, floor.z};
            frame.bodies.clear();
            for (uint32_t index = 0; index < body_count; ++index) {
                IBody*& body = bodies[index];
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
                    const uint32_t short_id = kinect_v2_short_tracking_id(id);
                    KinectBody& made = frame.bodies.emplace_back();
                    kinect_v2_body(mapped, short_id, index, made);
                    // The hands' state (open, a fist, lasso), which a v1 does not see.
                    int32_t state = 0;
                    if (SUCCEEDED(body->get_HandLeftState(&state))) made.hand_states[0] = uint8_t(state < 0 ? 0 : state);
                    state = 0;
                    if (SUCCEEDED(body->get_HandRightState(&state))) made.hand_states[1] = uint8_t(state < 0 ? 0 : state);
                    // Which edges of the picture cut it off: shown and logged only.
                    DWORD edges = 0;
                    if (SUCCEEDED(body->get_ClippedEdges(&edges))) made.clipped_edges = uint8_t(edges & kinect_edge::all);
                    if (recording) {
                        // All 25 joints as the sensor gave them, before smoothing.
                        KinectRecordBody& kept = record.bodies.emplace_back();
                        kept.sensor_index = index;
                        kept.tracking_id = id;
                        for (uint32_t j = 0; j < kinect_v2_joint_count; ++j)
                            kept.joints[j] = {{joints[j].position.x, joints[j].position.y, joints[j].position.z},
                                              joints[j].state < 0 ? 0 : int(joints[j].state)};
                        kept.hand_states = {int(made.hand_states[0]), int(made.hand_states[1])};
                        kept.clipped_edges = made.clipped_edges;
                    }
                }
                release(body);
            }
            {
                const auto clip_now = std::chrono::steady_clock::now();
                std::array<bool, 6> seen{};
                for (const KinectBody& tracked_body : frame.bodies) {
                    const size_t slot = tracked_body.sensor_index % 6;
                    seen[slot] = true;
                    if (tracked_body.clipped_edges == logged_edges[slot] ||
                        clip_now - logged_edges_at[slot] < std::chrono::seconds(1)) continue;
                    std::cerr << "NATIVE_KINECT_CLIPPED model=v2 body=" << tracked_body.sensor_index
                              << " edges=" << kinect_edges_text(tracked_body.clipped_edges)
                              << " mask=" << unsigned(tracked_body.clipped_edges) << '\n';
                    logged_edges[slot] = tracked_body.clipped_edges;
                    logged_edges_at[slot] = clip_now;
                }
                // A body that is gone starts again from "none" when it returns.
                for (size_t slot = 0; slot < seen.size(); ++slot)
                    if (!seen[slot]) logged_edges[slot] = 0;
            }
            if (recording) kinect_record_skeleton_frame(record);
            // Made depth: where the virtual v1 stands, placed by
            // the sensor's own view of the player (latched, or followed).
            KinectDepthView view;
            if (game_depth_) {
                const double seconds = relative_time ? double(relative_time) * 1e-7
                                                     : std::chrono::duration<double>(captured_at - began).count();
                depth_body_time_ = relative_time;
                game_view(frame, seconds);
                // The skeleton's own view: fit's skeleton is in real metres
                // (SFR_KINECT_V2_SKELETON_STRETCH, default 1.0) while its
                // depth is stretched by k; ideal's is the depth's unless
                // SFR_KINECT_V2_SKELETON_DISTANCE sets it apart (the title
                // never projects a skeleton into its depth; image_point
                // undoes this for the preview).
                view = viewer_.skeleton_view();
            }
            if (smooth_) smoother_.apply(frame);
            // The skeletons the game reads in the virtual camera's place too,
            // so that they fall on their own outlines in its depth image.
            if (game_depth_) kinect_depth_view_frame(view, frame);
            ++frames;
            if (!frame.bodies.empty()) ++tracked;
            {
                std::lock_guard guard(lock_);
                frame.number = latest_.number + 1;
                latest_ = frame;
                published_view_ = view;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - reported >= std::chrono::seconds(5)) {
                std::cerr << "NATIVE_KINECT model=v2 frames=" << frames << " with_body=" << tracked
                          << " bodies=" << frame.bodies.size();
                for (const KinectBody& cut : frame.bodies)
                    if (cut.clipped_edges)
                        std::cerr << " clipped" << cut.sensor_index << '=' << kinect_edges_text(cut.clipped_edges);
                // The preview's cameras, while it has them open.
                const uint64_t colour_frames = image_frames_[size_t(KinectImageKind::colour)].exchange(0);
                const uint64_t depth_frames = image_frames_[size_t(KinectImageKind::depth_and_player)].exchange(0);
                if (colour_frames || depth_frames)
                    std::cerr << " preview_colour_frames=" << colour_frames << " preview_depth_frames=" << depth_frames;
                // The overlay's points: placed by the mapper, by the lens.
                const uint64_t mapped = mapped_points_[0].exchange(0), estimated = mapped_points_[1].exchange(0);
                if (mapped || estimated)
                    std::cerr << " preview_mapped_points=" << mapped << " preview_estimated_points=" << estimated;
                if (game_depth_) {
                    // The game's depth images: how many were made, how much of
                    // the newest has a depth and how much of it is players,
                    // and where the virtual v1 stands.
                    constexpr double pixels = double(kinect_v1_depth_width * kinect_v1_depth_height);
                    const uint64_t made = game_frames_.exchange(0), lens = game_lens_frames_.exchange(0);
                    game_mapped_frames_.exchange(0);
                    std::cerr << " depth_synth_frames=" << made << " depth_valid="
                              << game_valid_.load(std::memory_order_relaxed) / pixels
                              << " depth_player_pixels=" << game_players_.load(std::memory_order_relaxed)
                              << " depth_view=" << viewer_.view().origin[0] << ',' << viewer_.view().origin[1] << ','
                              << viewer_.view().origin[2]
                              << " depth_latched=" << (viewer_.latched() ? 1 : 0);
                    if (viewer_.settings().mode == KinectDepthViewMode::ideal) std::cerr << ideal_description(viewer_.view());
                    // fit: the skeleton's own z scale (1 by default, the
                    // depth's k with "depth"); ideal with its own distance.
                    if (viewer_.settings().mode == KinectDepthViewMode::fit || viewer_.settings().skeleton_distance > 0)
                        std::cerr << " skeleton_view=" << view.origin[0] << ',' << view.origin[1] << ',' << view.origin[2]
                                  << " skeleton_z_scale=" << view.z_scale;
                    if (lens) std::cerr << " depth_lens_frames=" << lens;
                    if (KinectRawRecordFile* raw = kinect_record_raw_file())
                        std::cerr << " raw_frames=" << raw->frames_written();
                }
                // Recordings dropped for a slow disk, and what is queued.
                std::cerr << kinect_record_queue_status() << '\n';
                reported = now;
                frames = tracked = 0;
            }
        }
    }

    HMODULE library_ = nullptr;
    IKinectSensor* sensor_ = nullptr;
    IBodyFrameReader* reader_ = nullptr;
    // v2's SDK smooths nothing; the title does (its NuiTransformSmooth, done
    // natively for a real sensor's skeletons in nui_smooth_hooks.cpp), so the
    // joints go to the game as measured and are smoothed once, there.
    // SFR_KINECT_SMOOTH=1 (kinect_host_smoothing) smooths them here with the
    // same filter instead (kinect_smoothing.h), for sfr_kinect_probe --smooth;
    // the game then leaves its own out. Recordings take the joints before.
    bool smooth_ = kinect_host_smoothing();
    KinectJointSmoother smoother_;
    std::mutex lock_;
    KinectFrame latest_;
    uint64_t taken_ = 0;
    // The preview's cameras, under image_lock_.
    std::mutex image_lock_;
    IColorFrameReader* colour_reader_ = nullptr;
    IDepthFrameReader* depth_reader_ = nullptr;
    IBodyIndexFrameReader* index_reader_ = nullptr;
    bool images_open_ = false, has_body_index_ = false;
    bool colour_failure_logged_ = false, depth_failure_logged_ = false;
    std::chrono::steady_clock::time_point image_asked_{}, images_retry_at_{};
    std::array<KinectImage, 2> images_{};  // by KinectImageKind
    std::vector<uint8_t> colour_scratch_, body_index_;
    std::vector<uint16_t> depth_scratch_;
    std::array<std::atomic<uint64_t>, 2> image_frames_{};
    // The preview's coordinate mapper, under mapper_lock_, and how many of
    // its points it placed and how many the lens did, for the log.
    std::mutex mapper_lock_;
    ICoordinateMapper* mapper_ = nullptr;
    bool mapper_asked_ = false;
    std::array<std::atomic<uint64_t>, 2> mapped_points_{};
    std::atomic<bool> preview_common_{true};  // the preview's view (set_preview_common_view)
    // One call into the mapper at a time (the preview's thread and the
    // sensor's both use it).
    std::mutex map_lock_;
    // The Kinect v2 depth (kinect_v2_depth.h): the game's readers (open
    // before the worker starts, so game_depth_ never changes after), their
    // newest frames and times, the camera-space points, and the image made,
    // under game_image_lock_. The view the newest frame was moved by is
    // published_view_, under lock_.
    // Where the virtual v1 stands (every view; shared with the offline
    // re-synthesis, kinect_v2_resynth.h), on the sensor's thread.
    KinectDepthViewer viewer_{KinectDepthSettings::from_environment()};
    // Settings handed over by set_depth_settings, waiting for the sensor's
    // thread; and what ideal_snapshot answers, with the slots it is labelled by.
    std::mutex depth_settings_lock_;
    std::optional<KinectDepthSettings> pending_depth_settings_;
    std::mutex snapshot_lock_;
    KinectIdealSnapshot snapshot_;
    KinectPlayerSlots snapshot_slots_;  // the sensor's thread
    INT64 depth_body_time_ = 0;  // the newest body frame's RelativeTime, for a raw recording
    uint32_t raw_waited_ = 0;    // depth images made before a raw recording opened
    bool ideal_floor_logged_known_ = false, ideal_no_floor_logged_ = false;
    bool ideal_height_logged_known_ = false;
    std::chrono::steady_clock::time_point ideal_height_logged_at_{};
    // The fit view's stretch and where it came from (setting, sensor, or a
    // typical v2 until the sensor says), on the sensor's thread.
    float fit_focal_ = kinect_v2_typical_depth_focal;
    float fit_scale_ = kinect_v2_fit_scale(kinect_v2_typical_depth_focal);
    const char* fit_source_ = "typical";
    bool fit_lens_pending_ = false;
    uint32_t fit_lens_tries_ = 0;
    bool game_depth_ = false;
    std::string depth_failure_;  // why game_depth_ is false, set before the worker runs
    IDepthFrameReader* game_depth_reader_ = nullptr;
    IBodyIndexFrameReader* game_index_reader_ = nullptr;
    std::vector<uint16_t> game_millimetres_;
    std::vector<uint8_t> game_body_index_;
    std::vector<float> game_points_;
    INT64 game_depth_time_ = 0, game_index_time_ = 0, game_made_time_ = 0;
    std::mutex game_image_lock_;
    KinectImage game_image_;
    KinectDepthView published_view_;
    std::atomic<uint64_t> game_frames_{0}, game_mapped_frames_{0}, game_lens_frames_{0};
    std::atomic<uint32_t> game_valid_{0}, game_players_{0};
    // The newest depth image's quality, under depth_quality_lock_.
    std::mutex depth_quality_lock_;
    float depth_invalid_ = -1;
    uint64_t depth_quality_frame_ = 0;
    int64_t depth_quality_us_ = 0;
    std::jthread worker_;
};

}

std::unique_ptr<KinectSensor> open_kinect_v2(std::string* why) {
    auto sensor = std::make_unique<WindowsKinectV2>();
    if (!sensor->start(why)) return nullptr;
    return sensor;
}

}
