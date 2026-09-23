#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sfr {

// One picture from the camera, top-down, four bytes a pixel in the order the
// title's textures want (blue, green, red, alpha), alpha always 255.
struct CameraFrame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
    uint64_t number = 0;  // counts from one; 0 means nothing has arrived
};

// The webcam standing in for the Kinect sensor. What a camera delivers is
// rarely BGRA, so the two formats every webcam offers are converted here;
// a platform that can ask its capture stack to convert hands over BGRA.
enum class CameraPixels { bgra, yuy2, nv12 };

// Converts one camera picture into a frame. The source rows may be padded
// (stride is the bytes between rows; zero means the packed width). False when
// the size or the stride cannot hold the picture.
bool convert_camera_pixels(CameraPixels format, std::span<const uint8_t> source, uint32_t width, uint32_t height,
                           uint32_t stride, CameraFrame& frame);

// An open camera. Only one is opened; nothing else in the runtime holds one.
class CameraCapture {
public:
    virtual ~CameraCapture();
    // The newest picture, if one has arrived since the last call. False
    // leaves the frame alone, so a caller can keep showing the last one.
    virtual bool next(CameraFrame& frame) = 0;
    // The first camera the host offers, asked for about this size (the
    // nearest it offers is used). Null when there is none, when the platform
    // has no capture support, or when the player has not allowed it.
    static std::unique_ptr<CameraCapture> open(uint32_t width, uint32_t height);
    // The cameras the host offers, in the order open() numbers them
    // (SFR_CAMERA_DEVICE picks one). Empty where there is no capture stack.
    static std::vector<std::string> devices();
};

}
