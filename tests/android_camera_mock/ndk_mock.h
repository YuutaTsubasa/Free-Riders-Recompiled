#pragma once
// Host-side NDK boundary for exercising the actual Android capture backend.
// A closed session is poisoned, and device close drains callbacks before return.
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <future>
#include <thread>

constexpr int ACAMERA_OK = 0, AMEDIA_OK = 0, TEMPLATE_PREVIEW = 1;
constexpr int ACAMERA_LENS_FACING_BACK = 1, ACAMERA_LENS_FACING_FRONT = 0;
constexpr int ACAMERA_LENS_FACING = 1, ACAMERA_SENSOR_ORIENTATION = 2;
constexpr int ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS = 3, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE = 4;
constexpr int ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS = 5;
constexpr int ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT = 0, AIMAGE_FORMAT_YUV_420_888 = 35;
constexpr int SDL_ORIENTATION_PORTRAIT = 1, SDL_ORIENTATION_LANDSCAPE_FLIPPED = 2;
constexpr int SDL_ORIENTATION_PORTRAIT_FLIPPED = 3;
struct ACameraManager {};
struct ACameraMetadata {};
struct ACameraDevice;
struct ACameraCaptureSession;
struct ACaptureRequest { bool alive = true; };
struct ACameraOutputTarget {};
struct ACaptureSessionOutputContainer {};
struct ACaptureSessionOutput {};
struct ANativeWindow {};
struct AImageReader {};
struct AImage {};
struct ACameraIdList { int numCameras; const char** cameraIds; };
struct ACameraMetadata_const_entry {
    uint32_t tag = 0;
    uint8_t type = 0;
    uint32_t count = 0;
    union { const uint8_t* u8; const int32_t* i32; const float* f; const int64_t* i64; const double* d; } data{};
};
struct ACameraDevice_StateCallbacks {
    void* context;
    void (*onDisconnected)(void*, ACameraDevice*);
    void (*onError)(void*, ACameraDevice*, int);
};
struct ACameraCaptureSession_stateCallbacks {
    void* context;
    void (*onClosed)(void*, ACameraCaptureSession*);
    void (*onReady)(void*, ACameraCaptureSession*);
    void (*onActive)(void*, ACameraCaptureSession*);
};
struct ACameraCaptureSession {
    ACameraCaptureSession_stateCallbacks callbacks{};
    bool alive = true, repeating = false;
};
struct ACameraDevice {
    ACameraDevice_StateCallbacks callbacks{};
    ACameraCaptureSession* session = nullptr;
};
namespace mock_camera {
inline ACameraDevice* device = nullptr;
inline ACaptureRequest* request = nullptr;
inline uint64_t ticks = 0;
inline int opens = 0, closes = 0, invalid_session_calls = 0, freed_request_in_callback = 0;
inline bool fail_open = false, error_during_close = false, defer_ready = false;
inline bool retain_session_on_device_close = false;
inline std::vector<std::unique_ptr<ACaptureRequest>> requests;
inline std::vector<std::unique_ptr<ACameraCaptureSession>> sessions;
inline std::thread pending_callback;
inline std::promise<void> device_closing;
inline std::string last_id;
inline void closed(ACameraCaptureSession* session) {
    session->alive = false;
    session->repeating = false;
    session->callbacks.onClosed(session->callbacks.context, session);
}
inline void closed() { closed(device->session); }
inline void queue_closed() {
    auto* session = device->session;
    session->alive = false;
    session->repeating = false;
    device_closing = std::promise<void>{};
    auto closing = device_closing.get_future();
    pending_callback = std::thread([session, closing = std::move(closing)]() mutable {
        // Hold a queued close notification until teardown reaches device_close.
        // Session APIs must not be called on this already invalid handle.
        closing.wait();
        session->callbacks.onClosed(session->callbacks.context, session);
    });
}
inline void ready() {
    auto* session = device->session;
    session->callbacks.onReady(session->callbacks.context, session);
}
inline void lost(bool error) {
    if (error) device->callbacks.onError(device->callbacks.context, device, 4);
    else device->callbacks.onDisconnected(device->callbacks.context, device);
    closed();
}
inline void reset() {
    ticks = 0; opens = closes = invalid_session_calls = freed_request_in_callback = 0;
    fail_open = error_during_close = defer_ready = false;
    retain_session_on_device_close = false;
    requests.clear(); sessions.clear(); request = nullptr;
}
}
inline uint64_t SDL_GetTicks64() { return mock_camera::ticks; }
inline int SDL_GetDisplayOrientation(int) { return SDL_ORIENTATION_PORTRAIT; }
inline int SDL_AndroidRequestPermission(const char*) { return 1; }
inline ACameraManager* ACameraManager_create() { return new ACameraManager; }
inline void ACameraManager_delete(ACameraManager* p) { delete p; }
inline int ACameraManager_getCameraIdList(ACameraManager*, ACameraIdList** p) {
    static const char* ids[] = {"0", "selected"}; *p = new ACameraIdList{2, ids}; return 0;
}
inline void ACameraManager_deleteCameraIdList(ACameraIdList* p) { delete p; }
inline int ACameraManager_getCameraCharacteristics(ACameraManager*, const char*, ACameraMetadata** p) {
    *p = new ACameraMetadata; return 0;
}
inline void ACameraMetadata_free(ACameraMetadata* p) { delete p; }
inline int ACameraMetadata_getConstEntry(ACameraMetadata*, int, ACameraMetadata_const_entry*) { return -1; }
inline int ACameraManager_openCamera(ACameraManager*, const char* id, ACameraDevice_StateCallbacks* cb, ACameraDevice** p) {
    ++mock_camera::opens; mock_camera::last_id = id;
    if (mock_camera::fail_open) return -1;
    *p = mock_camera::device = new ACameraDevice{*cb}; return 0;
}
inline int ACameraDevice_close(ACameraDevice* p) {
    ++mock_camera::closes;
    if (mock_camera::pending_callback.joinable()) {
        mock_camera::device_closing.set_value();
        mock_camera::pending_callback.join();
    }
    if (mock_camera::error_during_close) {
        if (mock_camera::request && !mock_camera::request->alive) ++mock_camera::freed_request_in_callback;
        p->callbacks.onError(p->callbacks.context, p, 4);
    }
    if (p->session) {
        if (p->session->alive && !mock_camera::retain_session_on_device_close) mock_camera::closed();
    }
    delete p; mock_camera::device = nullptr; return 0;
}
inline int ACameraDevice_createCaptureRequest(ACameraDevice*, int, ACaptureRequest** p) {
    mock_camera::requests.push_back(std::make_unique<ACaptureRequest>());
    *p = mock_camera::request = mock_camera::requests.back().get(); return 0;
}
inline void ACaptureRequest_free(ACaptureRequest* p) { p->alive = false; }
inline int ACaptureRequest_addTarget(ACaptureRequest*, ACameraOutputTarget*) { return 0; }
inline int ACameraDevice_createCaptureSession(ACameraDevice* d, ACaptureSessionOutputContainer*, ACameraCaptureSession_stateCallbacks* cb, ACameraCaptureSession** p) {
    mock_camera::sessions.push_back(std::make_unique<ACameraCaptureSession>());
    *p = d->session = mock_camera::sessions.back().get();
    (*p)->callbacks = *cb;
    if (!mock_camera::defer_ready) mock_camera::ready();
    return 0;
}
inline int ACameraCaptureSession_setRepeatingRequest(ACameraCaptureSession* p, void*, int, ACaptureRequest** r, int*) {
    if (!p->alive) { ++mock_camera::invalid_session_calls; return -1; }
    if (!(*r)->alive) ++mock_camera::freed_request_in_callback;
    p->repeating = true; return 0;
}
inline int ACameraCaptureSession_stopRepeating(ACameraCaptureSession* p) {
    if (!p->alive) ++mock_camera::invalid_session_calls;
    p->repeating = false; return 0;
}
inline void ACameraCaptureSession_close(ACameraCaptureSession* p) {
    if (!p->alive) ++mock_camera::invalid_session_calls;
    else mock_camera::closed(p);
}
#define MOCK_RESOURCE(name) \
inline int name##_create(name** p) { *p = new name; return 0; } \
inline void name##_free(name* p) { delete p; }
MOCK_RESOURCE(ACaptureSessionOutputContainer)
#undef MOCK_RESOURCE
inline int ACaptureSessionOutput_create(ANativeWindow*, ACaptureSessionOutput** p) { *p = new ACaptureSessionOutput; return 0; }
inline void ACaptureSessionOutput_free(ACaptureSessionOutput* p) { delete p; }
inline int ACaptureSessionOutputContainer_add(ACaptureSessionOutputContainer*, ACaptureSessionOutput*) { return 0; }
inline int ACaptureSessionOutputContainer_remove(ACaptureSessionOutputContainer*, ACaptureSessionOutput*) { return 0; }
inline int ACameraOutputTarget_create(ANativeWindow*, ACameraOutputTarget** p) { *p = new ACameraOutputTarget; return 0; }
inline void ACameraOutputTarget_free(ACameraOutputTarget* p) { delete p; }
inline int AImageReader_new(int32_t, int32_t, int32_t, int32_t, AImageReader** p) { *p = new AImageReader; return 0; }
inline void AImageReader_delete(AImageReader* p) { delete p; }
inline int AImageReader_getWindow(AImageReader*, ANativeWindow** p) { static ANativeWindow window; *p = &window; return 0; }
inline int AImageReader_acquireLatestImage(AImageReader*, AImage** p) {
    if (!mock_camera::device || !mock_camera::device->session || !mock_camera::device->session->repeating) return -1;
    *p = new AImage; return 0;
}
inline void AImage_delete(AImage* p) { delete p; }
inline int AImage_getWidth(AImage*, int32_t* p) { *p = 2; return 0; }
inline int AImage_getHeight(AImage*, int32_t* p) { *p = 2; return 0; }
inline int AImage_getNumberOfPlanes(AImage*, int32_t* p) { *p = 3; return 0; }
inline int AImage_getPlaneData(AImage*, int plane, uint8_t** p, int* len) {
    static uint8_t y[] = {128, 128, 128, 128}, uv[] = {128};
    *p = plane ? uv : y; *len = plane ? 1 : 4; return 0;
}
inline int AImage_getPlaneRowStride(AImage*, int plane, int32_t* p) { *p = plane ? 1 : 2; return 0; }
inline int AImage_getPlanePixelStride(AImage*, int, int32_t* p) { *p = 1; return 0; }
