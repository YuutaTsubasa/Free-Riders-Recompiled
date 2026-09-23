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
    // One of the host's cameras, asked for about this size (the nearest it
    // offers is used). The camera is the one named, or the one
    // SFR_CAMERA_DEVICE names when nothing is passed. Null when there is
    // none, when the platform has no capture support, or when the player has
    // not allowed it.
    static std::unique_ptr<CameraCapture> open(uint32_t width, uint32_t height, const std::string& wanted = {});
    // The cameras the host offers, by name, in the order open() lists them.
    // SFR_CAMERA_DEVICE picks one of these names. Empty where there is no
    // capture stack.
    static std::vector<std::string> devices();
};

// Which of the listed cameras SFR_CAMERA_DEVICE asks for. A name is matched
// whole, then as a part of one (so a saved name still finds its camera when
// the host renames it slightly); a plain number is a place in the list, for
// the days before the cameras had names here. Anything unknown, or nothing
// asked for, is the first camera: hosts list their built-in one first.
// A list that changes while the game is away -- a phone camera closed, a
// capture card unplugged -- is the reason a number alone will not do.
size_t camera_device_index(const std::vector<std::string>& names, const std::string& wanted);
// The same, falling back to SFR_CAMERA_DEVICE when nothing is asked for.
size_t chosen_camera_device(const std::vector<std::string>& names, const std::string& wanted = {});

}
