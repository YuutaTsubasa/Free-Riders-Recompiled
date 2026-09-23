#include "camera_capture.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Whether a converted pixel is about the colour asked for: the integer
// BT.601 arithmetic is a couple of steps off an exact answer.
bool about(const sfr::CameraFrame& frame, uint32_t x, uint32_t y, int blue, int green, int red) {
    const uint8_t* p = frame.bgra.data() + (size_t(y) * frame.width + x) * 4;
    const auto near = [](int a, int b) { return a - b <= 3 && b - a <= 3; };
    return near(p[0], blue) && near(p[1], green) && near(p[2], red) && p[3] == 255;
}

void bgra_is_copied_with_opaque_alpha() {
    // Two by two, with a stride that pads each row by four bytes.
    const uint32_t stride = 2 * 4 + 4;
    std::vector<uint8_t> source(size_t(stride) * 2, 0x7f);
    for (uint32_t row = 0; row < 2; ++row)
        for (uint32_t x = 0; x < 2; ++x) {
            uint8_t* p = source.data() + row * stride + x * 4;
            p[0] = uint8_t(10 + x); p[1] = uint8_t(20 + row); p[2] = 30; p[3] = 0;
        }
    sfr::CameraFrame frame;
    require(sfr::convert_camera_pixels(sfr::CameraPixels::bgra, source, 2, 2, stride, frame),
            "a padded BGRA picture converts");
    require(frame.width == 2 && frame.height == 2 && frame.bgra.size() == 2 * 2 * 4, "the frame is packed");
    require(about(frame, 1, 0, 11, 20, 30) && about(frame, 0, 1, 10, 21, 30), "the padding is left out");
}

void yuy2_and_nv12_become_colours() {
    // Y=235 U=V=128 is white, Y=16 is black, and Y=81 U=90 V=240 is red.
    std::vector<uint8_t> yuy2{235, 128, 16, 128,   81, 90, 81, 240};  // two rows of two pixels
    sfr::CameraFrame frame;
    require(sfr::convert_camera_pixels(sfr::CameraPixels::yuy2, yuy2, 2, 2, 0, frame), "YUY2 converts");
    require(about(frame, 0, 0, 255, 255, 255), "Y 235 with neutral chroma is white");
    require(about(frame, 1, 0, 0, 0, 0), "Y 16 with neutral chroma is black");
    require(about(frame, 0, 1, 0, 0, 255) && about(frame, 1, 1, 0, 0, 255), "the chroma pair is shared");

    // Two wide and four tall: a luma plane, then two chroma rows, each shared
    // by two rows of pixels.
    std::vector<uint8_t> nv12{235, 16,  235, 16,   81, 81,  81, 81,
                              128, 128,  90, 240};
    require(sfr::convert_camera_pixels(sfr::CameraPixels::nv12, nv12, 2, 4, 0, frame), "NV12 converts");
    require(about(frame, 0, 0, 255, 255, 255), "the first luma with neutral chroma is white");
    require(about(frame, 1, 0, 0, 0, 0), "the second is black");
    require(about(frame, 0, 1, 255, 255, 255), "the first two rows share a chroma pair");
    require(about(frame, 0, 2, 0, 0, 255) && about(frame, 1, 3, 0, 0, 255),
            "the last two rows take the second chroma pair");
}

void sizes_that_cannot_hold_a_picture_are_refused() {
    std::vector<uint8_t> small(8, 0);
    sfr::CameraFrame frame;
    require(!sfr::convert_camera_pixels(sfr::CameraPixels::bgra, small, 2, 2, 0, frame), "too few bytes are refused");
    require(!sfr::convert_camera_pixels(sfr::CameraPixels::bgra, small, 2, 2, 4, frame), "a short stride is refused");
    require(!sfr::convert_camera_pixels(sfr::CameraPixels::yuy2, small, 3, 2, 0, frame), "an odd width is refused");
    require(!sfr::convert_camera_pixels(sfr::CameraPixels::bgra, small, 0, 2, 0, frame), "an empty picture is refused");
}

void a_camera_is_found_by_its_name() {
    const std::vector<std::string> cameras = {"Live Gamer Portable 2", "e2eSoft iVCam #2", "Integrated Webcam"};
    require(sfr::camera_device_index(cameras, "e2eSoft iVCam #2") == 1, "a name is found");
    require(sfr::camera_device_index(cameras, "Integrated") == 2, "part of a name is enough");
    require(sfr::camera_device_index(cameras, "DroidCam") == 0, "a camera that has gone falls back to the first");
    require(sfr::camera_device_index(cameras, "") == 0, "asking for nothing takes the first");
    require(sfr::camera_device_index({}, "Integrated Webcam") == 0, "no cameras, nothing to choose");
    // Digits are a place in the list, even where a name also holds them.
    require(sfr::camera_device_index(cameras, "2") == 2, "an old numbered setting still means the third camera");
    require(sfr::camera_device_index(cameras, "7") == 0, "a place the host does not have falls back to the first");
}
}

int main() {
    try {
        bgra_is_copied_with_opaque_alpha();
        yuy2_and_nv12_become_colours();
        sizes_that_cannot_hold_a_picture_are_refused();
        a_camera_is_found_by_its_name();
        std::cout << "Camera capture checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
