#pragma once
#include "kinect_sensor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sfr {
// A window of what a real Kinect sees, for placing and tilting it before
// playing: the colour camera and the depth camera, each with the skeletons
// the sensor tracks drawn over it, and whether each body's feet are in view.
// It opens the sensor itself (the SDK allows one session a process) and
// shares it for the launcher's tilt buttons; closing the window, or
// destroying this, closes the sensor so the game can open it. Windows only:
// elsewhere open() returns null.
class KinectPreviewWindow {
public:
    // model is the launcher's choice: 1 the Xbox 360 Kinect (a v2 stands in
    // when only that opens), 2 the Kinect v2 only (KinectSensor::open_selected).
    // depth is the launcher's Kinect v2 depth view and Recompiled's tuning:
    // the window's view list starts on it and its sensor makes that depth
    // (KinectSensor::set_depth_settings); another view picked in the window
    // stays the window's own and never changes the launcher's settings.
    static std::unique_ptr<KinectPreviewWindow> open(bool chinese, int model = 1, const KinectDepthChoice& depth = {});
    // The launcher's choice again (every frame while it is open; any thread):
    // new tuning is used at once, and a view changed in the launcher is shown.
    void set_depth_choice(const KinectDepthChoice& depth);
    // Where the Recompiled view's virtual v1 and the players are now
    // (KinectSensor::ideal_snapshot); false while there is no Kinect v2
    // making depth.
    bool ideal_snapshot(KinectIdealSnapshot& snapshot) const;
    // The same window over the game's own sensor, for the menu's Kinect
    // adjustment (kinect_tuner.h): it neither opens nor closes the sensor and
    // never takes its skeleton frames, which the game reads; the game hands
    // it each one with show_frame. It comes to the front over the game's
    // window when opened (and raise() brings it back), without staying on
    // top, and speaks the system's language.
    static std::unique_ptr<KinectPreviewWindow> open_over(KinectSensor& sensor);
    void show_frame(const KinectFrame& frame);
    ~KinectPreviewWindow();
    bool closed() const;
    // Closes it as its close box does (the game's window in front again).
    void close();
    // Brings it in front again (asked for while already open).
    void raise();
    // The open sensor, or null while it is opening or when there is none.
    std::shared_ptr<KinectSensor> sensor() const;
    // Why no sensor opened (KinectSensor::open's reason, "no-runtime",
    // "no-sensor", ...), or empty while it opens or once it has.
    std::string failure() const;
    struct Impl;
private:
    explicit KinectPreviewWindow(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

// What the preview has seen so far, step by step, so that it can say where
// it stops rather than only that it does: the runtime (the SDK), the sensor,
// frames arriving from it, and a body tracked in them. Each step is only
// worth checking once the one before it holds; a later step stays waiting
// while an earlier one fails. A step holds when it is really working, not
// when something is installed: a sensor that opened but sends nothing has
// not passed the frames step.
enum class KinectStepState : uint8_t { waiting, passed, failed };
enum class KinectStep : uint8_t { runtime, sensor, frames, body, count };
struct KinectReadinessInput {
    bool opened = false;          // KinectSensor::open succeeded
    std::string failure;          // its reason when it did not; empty while it opens
    double seconds_open = 0;      // since it opened
    double seconds_since_frame = -1;  // since a skeleton frame or an image last arrived; -1 for never
    size_t bodies = 0;            // tracked in the newest frame
};
struct KinectReadiness {
    std::array<KinectStepState, size_t(KinectStep::count)> steps{};
    // The first step that failed, or KinectStep::count when none has.
    KinectStep first_failed = KinectStep::count;
    bool ready() const { return steps[size_t(KinectStep::body)] == KinectStepState::passed; }
};
KinectReadiness kinect_readiness(const KinectReadinessInput& input);
// Frames stop counting as arriving after this long without one, and a sensor
// open this long without any has failed the frames step.
constexpr double kinect_frame_timeout_seconds = 1.5, kinect_first_frame_seconds = 3.0;

// Where a point of the sensor's skeleton space (metres) falls in one of its
// images: the depth camera's 320x240 exactly (the SDK's
// NuiTransformSkeletonToDepthImage), the colour camera's 640x480 roughly
// (its nominal focal length; the two cameras sit a few centimetres apart).
// KinectImagePoint is kinect_sensor.h's. A sensor that places points itself
// (KinectSensor::image_point, a Kinect v2) is asked first.
KinectImagePoint kinect_depth_point(const std::array<float, 3>& p);
KinectImagePoint kinect_colour_point(const std::array<float, 3>& p);

// The body the game is given while the adjustment window is open: as it
// stood, its arms hanging at its sides, so no menu cursor is raised and a
// hand left resting on a button does not keep pressing it.
void kinect_arms_down(KinectBody& body);
// The same for a skeleton without tracking states (the webcam's player).
void arms_down(SkeletonJoints& joints);
}
