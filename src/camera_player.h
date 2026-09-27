#pragma once
#include "pose_skeleton.h"

#include <memory>

namespace sfr {

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
    bool joints(SkeletonJoints& joints);
    // Whether a body has ever been found: until one is, the title is better
    // off with the pad's emulated player than with an empty skeleton.
    bool tracking() const;
    // Whether the player has left: a body was found once, and none for
    // SFR_CAMERA_LEAVE_SECONDS (1.5 by default). Their slot is then empty, as
    // a real sensor's would be, so the title asks for a player and the next
    // one to step in takes it over (a relay). Zero never leaves: the last
    // pose stays, for a camera that loses the player now and then.
    bool left() const;
    // The same rule, on its own for testing: seconds since the last body,
    // and the setting.
    static bool left_after(double seconds_without_body, double leave_seconds);

    struct Impl;
private:
    explicit CameraPlayer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}
