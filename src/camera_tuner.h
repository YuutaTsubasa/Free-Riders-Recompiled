#pragma once
#include "camera_player.h"

#include <array>
#include <cstddef>
#include <memory>

namespace sfr {

// The menu's Kinect adjustment with a webcam instead of a sensor
// (kinect_tuner.h): a window over the game's camera showing what it sees,
// the body found in it, and a step at a time what is still missing for play,
// the way the Kinect preview does (kinect_preview.h). A webcam has no motor,
// so the hints say where to stand and where to put the camera.
enum class CameraStep : uint8_t { camera, pictures, body, whole_body, count };
enum class CameraStepState : uint8_t { waiting, passed, failed };
struct CameraReadinessInput {
    bool opened = false;              // the game's camera is open
    double seconds_open = 0;          // since the window opened
    double seconds_since_picture = -1;// -1: none yet
    bool motion = true;               // a pose model looks at the pictures
    bool found = false;               // a body in the newest picture
    bool whole_body = false;          // shoulders to ankles inside it
};
struct CameraReadiness {
    std::array<CameraStepState, size_t(CameraStep::count)> steps{};
    CameraStep first_failed = CameraStep::count;
    bool ready() const { return steps[size_t(CameraStep::whole_body)] == CameraStepState::passed; }
};
CameraReadiness camera_readiness(const CameraReadinessInput& input);

// Pictures stop counting as arriving after this long without one, and a
// window open this long without any has failed the pictures step.
constexpr double camera_picture_timeout_seconds = 1.5, camera_first_picture_seconds = 3.0;

// Whether the body is in the picture from the shoulders to the ankles, each
// of those points seen (score at least 0.5) and a twentieth of the picture
// away from its edges, and if not, what would help: stepping back (a point
// off the top or the bottom, or not seen at all) or moving towards the
// middle (a point off a side).
enum class CameraAdvice : uint8_t { none, step_back, to_the_middle };
struct CameraFraming {
    bool whole_body = false;
    CameraAdvice advice = CameraAdvice::none;
};
CameraFraming camera_framing(const PoseLandmarks& body, uint32_t width, uint32_t height);

class CameraTunerWindow {
public:
    // Over the game's camera player; null where there are no windows.
    static std::unique_ptr<CameraTunerWindow> open_over(CameraPlayer& player);
    ~CameraTunerWindow();
    bool closed() const;
    // Closes it as its close box does (the game's window in front again).
    void close();
    // Brings it in front again (asked for while already open).
    void raise();
    struct Impl;
private:
    explicit CameraTunerWindow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}
