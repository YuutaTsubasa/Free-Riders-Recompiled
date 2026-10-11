// The webcam adjustment window (camera_tuner.h) without the game: opens the
// camera the launcher would (SFR_CAMERA_DEVICE), with the pose model when it
// is there and the picture alone when it is not, and shows the window until
// it is closed. Settings made in it are kept as in the game.
#include "camera_player.h"
#include "camera_tuner.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
#ifdef _WIN32
    _putenv("SFR_CAMERA=motion");
#endif
    auto player = sfr::CameraPlayer::start();
    if (!player) {
        std::cerr << "camera_tuner_probe: no pose model, showing the picture only\n";
#ifdef _WIN32
        _putenv("SFR_CAMERA=picture");
#endif
        player = sfr::CameraPlayer::start();
    }
    if (!player) {
        std::cerr << "camera_tuner_probe: no camera could be opened\n";
        return 1;
    }
    const auto window = sfr::CameraTunerWindow::open_over(*player);
    if (!window) {
        std::cerr << "camera_tuner_probe: the window needs Windows\n";
        return 1;
    }
    while (!window->closed()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}
