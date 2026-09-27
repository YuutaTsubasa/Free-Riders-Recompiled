// The phone's own cameras on Android, through the NDK's Camera2 API and an
// image reader: YUV_420_888 pictures, turned upright for the way the screen
// is held and converted in camera_capture.cpp. The CAMERA permission is asked
// for when a camera is first opened (SDL shows the system's question and
// waits for the answer), so a player who never turns the camera on is never
// asked.
#include "camera_capture.h"

#include <SDL.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace sfr {
namespace {

// What the player chooses between: every camera the system lists, with
// which way it faces and how wide it sees. The name carries the camera's id,
// so a saved choice finds the same lens again.
struct CameraInfo {
    std::string id, name;
    bool front = false;
    int32_t orientation = 0;  // SENSOR_ORIENTATION
};

std::vector<CameraInfo> list_cameras(ACameraManager* manager) {
    std::vector<CameraInfo> cameras;
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids) return cameras;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata* metadata = nullptr;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) != ACAMERA_OK) continue;
        CameraInfo info;
        info.id = ids->cameraIds[i];
        ACameraMetadata_const_entry entry{};
        uint8_t facing = ACAMERA_LENS_FACING_BACK;
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK && entry.count)
            facing = entry.data.u8[0];
        info.front = facing == ACAMERA_LENS_FACING_FRONT;
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_ORIENTATION, &entry) == ACAMERA_OK && entry.count)
            info.orientation = entry.data.i32[0];
        // How wide it sees across the longer side: the sensor's width over
        // its shortest focal length. An ultra-wide lens is the one that lets
        // the player stand closest.
        int degrees = 0;
        ACameraMetadata_const_entry focal{}, size{};
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &focal) == ACAMERA_OK &&
            focal.count &&
            ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &size) == ACAMERA_OK &&
            size.count >= 2 && focal.data.f[0] > 0) {
            float shortest = focal.data.f[0];
            for (uint32_t f = 1; f < focal.count; ++f) shortest = std::fmin(shortest, focal.data.f[f]);
            const float width = std::fmax(size.data.f[0], size.data.f[1]);
            degrees = int(std::lround(2.0 * std::atan(width / (2.0 * shortest)) * 180.0 / 3.14159265358979));
        }
        const char* side = facing == ACAMERA_LENS_FACING_FRONT ? "Front" : facing == ACAMERA_LENS_FACING_BACK ? "Back"
                                                                                                             : "External";
        info.name = std::string(side) + " camera " + info.id;
        if (degrees > 0) info.name += " (" + std::to_string(degrees) + "\xC2\xB0)";
        cameras.push_back(std::move(info));
        ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(ids);
    return cameras;
}

// The YUV size nearest the one asked for among those the camera offers.
std::pair<int32_t, int32_t> nearest_size(ACameraManager* manager, const std::string& id, uint32_t width,
                                         uint32_t height) {
    std::pair<int32_t, int32_t> best{int32_t(width), int32_t(height)};
    ACameraMetadata* metadata = nullptr;
    if (ACameraManager_getCameraCharacteristics(manager, id.c_str(), &metadata) != ACAMERA_OK) return best;
    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) ==
        ACAMERA_OK) {
        const double wanted = double(width) * height;
        double closest = -1;
        // Four values each: format, width, height, and whether it is an input.
        for (uint32_t i = 0; i + 3 < entry.count; i += 4) {
            const int32_t* c = entry.data.i32 + i;
            if (c[0] != AIMAGE_FORMAT_YUV_420_888 || c[3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
                continue;
            const double difference = std::fabs(double(c[1]) * c[2] - wanted);
            if (closest < 0 || difference < closest) {
                closest = difference;
                best = {c[1], c[2]};
            }
        }
    }
    ACameraMetadata_free(metadata);
    return best;
}

// How far the screen is turned from the device's natural orientation. The
// game holds it in landscape, so an unknown answer is taken as that.
uint32_t display_rotation() {
    switch (SDL_GetDisplayOrientation(0)) {
    case SDL_ORIENTATION_PORTRAIT: return 0;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return 270;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED: return 180;
    default: return 90;
    }
}

void device_disconnected(void*, ACameraDevice*) {
    std::cerr << "NATIVE_CAMERA android=disconnected\n";
}
void device_error(void*, ACameraDevice*, int error) {
    std::cerr << "NATIVE_CAMERA android=error code=" << error << '\n';
}
void session_state(void*, ACameraCaptureSession*) {}

class AndroidCamera final : public CameraCapture {
public:
    ~AndroidCamera() override {
        if (session_) {
            ACameraCaptureSession_stopRepeating(session_);
            ACameraCaptureSession_close(session_);
        }
        if (request_) ACaptureRequest_free(request_);
        if (target_) ACameraOutputTarget_free(target_);
        if (container_ && output_) ACaptureSessionOutputContainer_remove(container_, output_);
        if (output_) ACaptureSessionOutput_free(output_);
        if (container_) ACaptureSessionOutputContainer_free(container_);
        if (device_) ACameraDevice_close(device_);
        if (reader_) AImageReader_delete(reader_);
        if (manager_) ACameraManager_delete(manager_);
    }

    bool start(ACameraManager* manager, const CameraInfo& camera, uint32_t width, uint32_t height) {
        manager_ = manager;
        front_ = camera.front;
        orientation_ = uint32_t(camera.orientation);
        const auto [reader_width, reader_height] = nearest_size(manager_, camera.id, width, height);
        if (AImageReader_new(reader_width, reader_height, AIMAGE_FORMAT_YUV_420_888, 3, &reader_) != AMEDIA_OK)
            return fail("reader");
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(reader_, &window) != AMEDIA_OK || !window) return fail("window");
        device_callbacks_ = {this, device_disconnected, device_error};
        if (ACameraManager_openCamera(manager_, camera.id.c_str(), &device_callbacks_, &device_) != ACAMERA_OK)
            return fail("open");
        if (ACaptureSessionOutputContainer_create(&container_) != ACAMERA_OK ||
            ACaptureSessionOutput_create(window, &output_) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_add(container_, output_) != ACAMERA_OK ||
            ACameraOutputTarget_create(window, &target_) != ACAMERA_OK ||
            ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &request_) != ACAMERA_OK ||
            ACaptureRequest_addTarget(request_, target_) != ACAMERA_OK)
            return fail("request");
        session_callbacks_ = {this, session_state, session_state, session_state};
        if (ACameraDevice_createCaptureSession(device_, container_, &session_callbacks_, &session_) != ACAMERA_OK)
            return fail("session");
        if (ACameraCaptureSession_setRepeatingRequest(session_, nullptr, 1, &request_, nullptr) != ACAMERA_OK)
            return fail("repeat");
        std::cerr << "NATIVE_CAMERA android=open camera=\"" << camera.name << "\" size=" << reader_width << 'x'
                  << reader_height << " orientation=" << orientation_ << '\n';
        return true;
    }

    bool next(CameraFrame& frame) override {
        AImage* image = nullptr;
        // Non-blocking: no new picture leaves the last one in place.
        if (AImageReader_acquireLatestImage(reader_, &image) != AMEDIA_OK || !image) return false;
        bool converted = false;
        int32_t width = 0, height = 0, planes = 0;
        AImage_getWidth(image, &width);
        AImage_getHeight(image, &height);
        AImage_getNumberOfPlanes(image, &planes);
        if (planes == 3 && width > 0 && height > 0) {
            uint8_t* data[3] = {};
            int length[3] = {};
            int32_t row[3] = {}, pixel[3] = {};
            bool read = true;
            for (int plane = 0; plane < 3; ++plane)
                read = read && AImage_getPlaneData(image, plane, &data[plane], &length[plane]) == AMEDIA_OK &&
                       AImage_getPlaneRowStride(image, plane, &row[plane]) == AMEDIA_OK &&
                       AImage_getPlanePixelStride(image, plane, &pixel[plane]) == AMEDIA_OK;
            if (read && row[1] == row[2] && pixel[1] == pixel[2]) {
                CameraYuvPlanes yuv;
                yuv.y = {data[0], size_t(length[0])};
                yuv.u = {data[1], size_t(length[1])};
                yuv.v = {data[2], size_t(length[2])};
                yuv.y_row = uint32_t(row[0]);
                yuv.uv_row = uint32_t(row[1]);
                yuv.uv_pixel = uint32_t(pixel[1]);
                converted = convert_camera_yuv420(yuv, uint32_t(width), uint32_t(height),
                                                  camera_upright_rotation(orientation_, front_, display_rotation()),
                                                  frame);
            }
        }
        AImage_delete(image);
        if (converted) frame.number = ++number_;
        return converted;
    }

private:
    bool fail(const char* step) {
        std::cerr << "NATIVE_CAMERA android=failed step=" << step << '\n';
        return false;
    }

    ACameraManager* manager_ = nullptr;
    AImageReader* reader_ = nullptr;
    ACameraDevice* device_ = nullptr;
    ACaptureSessionOutputContainer* container_ = nullptr;
    ACaptureSessionOutput* output_ = nullptr;
    ACameraOutputTarget* target_ = nullptr;
    ACaptureRequest* request_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    ACameraDevice_StateCallbacks device_callbacks_{};
    ACameraCaptureSession_stateCallbacks session_callbacks_{};
    bool front_ = false;
    uint32_t orientation_ = 0;
    uint64_t number_ = 0;
};

}

std::vector<std::string> CameraCapture::devices() {
    std::vector<std::string> names;
    ACameraManager* manager = ACameraManager_create();
    if (!manager) return names;
    for (const auto& camera : list_cameras(manager)) names.push_back(camera.name);
    ACameraManager_delete(manager);
    return names;
}

std::unique_ptr<CameraCapture> CameraCapture::open(uint32_t width, uint32_t height, const std::string& wanted) {
    // Asked once per run; SDL waits for the player's answer.
    static std::atomic<int> allowed{-1};
    if (allowed.load() < 0) allowed.store(SDL_AndroidRequestPermission("android.permission.CAMERA") ? 1 : 0);
    if (!allowed.load()) {
        std::cerr << "NATIVE_CAMERA android=not-allowed\n";
        return nullptr;
    }
    ACameraManager* manager = ACameraManager_create();
    if (!manager) return nullptr;
    const auto cameras = list_cameras(manager);
    if (cameras.empty()) {
        ACameraManager_delete(manager);
        std::cerr << "NATIVE_CAMERA android=no-camera\n";
        return nullptr;
    }
    std::vector<std::string> names;
    for (const auto& camera : cameras) names.push_back(camera.name);
    const CameraInfo& chosen = cameras[chosen_camera_device(names, wanted)];
    auto camera = std::make_unique<AndroidCamera>();
    if (!camera->start(manager, chosen, width, height)) return nullptr;  // the camera frees the manager
    return camera;
}

}
