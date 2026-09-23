#pragma once
#include "pose_estimator.h"

#include <array>
#include <cstdint>

namespace sfr {

// The model reads every picture on its own, so a point that is not moving
// still wanders a pixel or two between pictures. The cursor answers to about
// 2600 pixels per metre of hand, and a picture pixel is several millimetres,
// so that wandering is what makes the cursor float.
//
// The 1 Euro filter (Casiez, Roussel and Vogel, 2012): a low-pass whose
// cutoff rises with how fast the point is moving. Standing still is smoothed
// hard, so the cursor settles; a real movement raises the cutoff at once, so
// pointing does not lag behind the hand. Two numbers decide it -- the cutoff
// at rest, and how much speed raises it.
class PoseSmoothing {
public:
    // Hz, and Hz per pixel a second. A cutoff of zero passes the points
    // through untouched. The defaults are SFR_POSE_SMOOTHING and
    // SFR_POSE_SMOOTHING_BETA.
    static PoseSmoothing from_environment();
    PoseSmoothing(float cutoff_at_rest, float speed_coefficient);

    // Smooths in place. The interval is the time since the picture before
    // this one; a first picture, or one after a gap, starts the filter again.
    void smooth(PoseLandmarks& landmarks, double interval_seconds);
    void forget();

private:
    struct Axis {
        float value = 0, speed = 0;
        bool started = false;
    };
    float cutoff_at_rest_, speed_coefficient_;
    std::array<std::array<Axis, 2>, pose_point::count> points_{};
};

}
