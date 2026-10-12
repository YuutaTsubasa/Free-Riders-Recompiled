#pragma once
#include "camera_capture.h"
#include "pose_estimator.h"
#include "pose_skeleton.h"

#include <memory>

namespace sfr {
// What the camera sees, for the adjustment window (camera_tuner.h): a
// picture as the camera gave it and the body found in it, in its pixels.
struct CameraView {
    CameraFrame picture;          // number 0: nothing yet
    PoseLandmarks body{};
    bool found = false;           // a body was found in this picture
    bool motion = false;          // a pose model is looking (SFR_CAMERA=motion)
    bool mirrored = false;        // the camera hands over a mirrored picture
};

struct CameraTrackingStatus {
    bool detected = false;
    double observation_age_ms = -1, pose_age_ms = -1;
};

// The camera standing in for the sensor, as the title's Kinect player. It
// reads the camera and runs the pose model on a thread of its own, so a
// frame the game draws never waits for either.
//
// SFR_CAMERA=picture opens the camera without the model (nothing drives the
// player); SFR_CAMERA=motion runs the model as well.
class CameraPlayer {
public:
    ~CameraPlayer();
    // Null when SFR_CAMERA is off, or the camera or the model is missing.
    static std::unique_ptr<CameraPlayer> start();
    // The joints of the last body found, if there has been one since the
    // last call. False leaves them alone, so the player keeps its last pose.
    // Optional freshness metadata is always sampled, even if no new joints
    // are returned, so source selection can reject a frozen/lost stream.
    bool joints(SkeletonJoints& joints, CameraTrackingStatus* status = nullptr);
    // Whether a body has ever been found: until one is, the title is better
    // off with the pad's emulated player than with an empty skeleton.
    bool tracking() const;
    // Whether the title is in its menus (not racing), where the hands are a
    // cursor and are steadied (MenuHandSteadying).
    void set_in_menu(bool in_menu);
    // While on, each picture is kept for view(); off, none is copied.
    void share_view(bool on);
    // The newest picture, if one newer than view.picture.number has been
    // kept. False leaves the view alone.
    bool view(CameraView& view);
    // The camera's own controls (zoom, exposure, gain), for the window.
    bool control_range(CameraControl control, CameraControlRange& range);
    bool control(CameraControl control, CameraControlValue& value);
    bool set_control(CameraControl control, const CameraControlValue& value);

    struct Impl;
private:
    explicit CameraPlayer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}
