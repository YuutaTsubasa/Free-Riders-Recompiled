// Opens a Kinect v1 through libfreenect (macOS, #305) and counts what it
// delivers, so that a Mac's Kinect can be checked without starting the game:
//   sfr_freenect_probe [seconds]
// Depth comes in millimetres in the depth camera's own view (640x480,
// FREENECT_DEPTH_MM), colour as RGB (640x480). Built against fakenect
// instead (sfr_fakenect_probe), it plays a recording back from FAKENECT_PATH.
#include <libfreenect.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
std::atomic<uint64_t> depth_frames{0}, colour_frames{0}, valid_pixels{0};
std::atomic<uint32_t> nearest_mm{0};

void on_depth(freenect_device*, void* data, uint32_t) {
    const auto* pixels = static_cast<const uint16_t*>(data);
    uint64_t valid = 0;
    uint32_t nearest = 0;
    for (int i = 0; i < 640 * 480; ++i)
        if (const uint16_t mm = pixels[i]; mm != FREENECT_DEPTH_MM_NO_VALUE) {
            ++valid;
            if (!nearest || mm < nearest) nearest = mm;
        }
    valid_pixels = valid;
    nearest_mm = nearest;
    ++depth_frames;
}

void on_colour(freenect_device*, void*, uint32_t) { ++colour_frames; }
}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    freenect_context* context = nullptr;
    if (freenect_init(&context, nullptr) < 0) {
        std::cout << "no libfreenect context\n";
        return 1;
    }
    freenect_set_log_level(context, FREENECT_LOG_WARNING);
    freenect_select_subdevices(context, freenect_device_flags(FREENECT_DEVICE_CAMERA));
    const int count = freenect_num_devices(context);
    std::cout << "Kinect v1 devices: " << count << '\n';
    if (count < 1) {
        freenect_shutdown(context);
        return 1;
    }
    freenect_device* device = nullptr;
    if (freenect_open_device(context, &device, 0) < 0) {
        std::cout << "could not open the Kinect\n";
        freenect_shutdown(context);
        return 1;
    }
    const freenect_frame_mode depth_mode = freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_MM);
    const freenect_frame_mode colour_mode = freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_RGB);
    if (!depth_mode.is_valid || !colour_mode.is_valid || freenect_set_depth_mode(device, depth_mode) < 0 ||
        freenect_set_video_mode(device, colour_mode) < 0) {
        std::cout << "the Kinect refused 640x480 depth (mm) and RGB\n";
        freenect_close_device(device);
        freenect_shutdown(context);
        return 1;
    }
    freenect_set_depth_callback(device, on_depth);
    freenect_set_video_callback(device, on_colour);
    freenect_start_depth(device);
    freenect_start_video(device);
    std::cout << "reading for " << seconds << " s; stand in front of it\n";
    const auto start = std::chrono::steady_clock::now(), end = start + std::chrono::seconds(seconds);
    auto shown = start;
    while (std::chrono::steady_clock::now() < end && freenect_process_events(context) >= 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - shown >= std::chrono::seconds(1)) {
            shown = now;
            std::cout << "depth " << depth_frames << " colour " << colour_frames << " valid "
                      << double(valid_pixels) / (640.0 * 480.0) << " nearest_mm " << nearest_mm << std::endl;
        }
    }
    freenect_stop_depth(device);
    freenect_stop_video(device);
    freenect_close_device(device);
    freenect_shutdown(context);
    std::cout << "frames: depth " << depth_frames << ", colour " << colour_frames << '\n';
    return depth_frames && colour_frames ? 0 : 1;
}
