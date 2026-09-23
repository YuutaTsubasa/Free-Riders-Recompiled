// No capture stack on this platform yet: the camera setting has no effect,
// and the title is told there is no sensor, as it is without one.
#include "camera_capture.h"

#include <iostream>

namespace sfr {
std::vector<std::string> CameraCapture::devices() { return {}; }

std::unique_ptr<CameraCapture> CameraCapture::open(uint32_t, uint32_t) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::cerr << "NATIVE_CAMERA unavailable=no-capture-on-this-platform\n";
    }
    return nullptr;
}
}
