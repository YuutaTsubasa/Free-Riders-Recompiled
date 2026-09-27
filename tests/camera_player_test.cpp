#include "camera_player.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        // A relay: the player walks out, and after a moment their slot is free.
        require(!sfr::CameraPlayer::left_after(0.5, 1.5), "a moment out of the picture is not leaving");
        require(sfr::CameraPlayer::left_after(1.5, 1.5), "out of the picture long enough is leaving");
        require(!sfr::CameraPlayer::left_after(60.0, 0.0), "a setting of zero never leaves");
        // Without SFR_CAMERA there is no camera player at all.
#ifdef _WIN32
        _putenv_s("SFR_CAMERA", "");
#else
        unsetenv("SFR_CAMERA");
#endif
        require(sfr::CameraPlayer::start() == nullptr, "the camera is off unless asked for");
        std::cout << "Camera player checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
