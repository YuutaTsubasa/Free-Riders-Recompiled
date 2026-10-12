#pragma once
#include "pose_estimator.h"
#include "pose_skeleton.h"

#include <array>
#include <cstdint>

namespace sfr {

// The model reads every picture on its own, and when a hand passes in front
// of the body, or an arm leaves the picture, it still names a place for it:
// a guess, often on the wrong side of the body or half a metre away. Played
// as it is, that guess is a gesture nobody made.
//
// So each limb point (elbows, wrists, knees, ankles) is checked against what
// a body can do before it is believed:
//   - the model's own confidence in it (models that give a world pose
//     report a visibility; the older picture-only ones are not asked);
//   - the length of the bone to it, against the length that bone has had for
//     this player: a forearm does not grow by half from one picture to the
//     next;
//   - how far it moved since the last picture: farther than a hand can go is
//     a jump, unless it stays there, which a real fast movement does.
// A point that fails is held where it was relative to the joint it hangs
// from, carried on by the speed it had, slowing, so a hidden hand moves with
// the arm and the body it belongs to. After a quarter of a second the model
// is believed again whatever it says: holding longer would freeze a limb that
// really is where the model puts it.
//
// It runs on the points as the model gives them, before PoseSmoothing, so a
// jump is taken out instead of being smoothed into a slide.
class PoseStabilizer {
public:
    // SFR_POSE_STABILIZE=0 turns it off.
    static PoseStabilizer from_environment();
    explicit PoseStabilizer(bool enabled = true) : enabled_(enabled) {}

    // Checks and holds in place. The interval is the time since the picture
    // before; a first picture, or one after a gap, starts again.
    void stabilize(PoseLandmarks& landmarks, double interval_seconds);
    void forget();

    // How many points were held in the last picture, for the log.
    uint32_t held() const { return held_; }

    // A world model's visibility below this is not believed.
    static constexpr float min_score = 0.5f;
    // A bone longer than its usual length by this much is a wrong point.
    static constexpr float max_stretch = 1.6f;
    // The farthest a point moves in one picture, in metres for world models
    // and shoulder widths for picture models (about 13 m/s at 30 pictures a
    // second: faster than a punch).
    static constexpr float max_jump_metres = 0.45f, max_jump_spans = 1.25f;
    // A jump that is still there after this many pictures is a movement.
    static constexpr uint32_t max_rejected_in_a_row = 2;
    static constexpr double max_hold_seconds = 0.25, speed_decay_seconds = 0.1;

private:
    struct Limb {
        uint32_t point, parent;
    };
    struct Memory {
        bool known = false;         // a believed position to hold from
        bool holding = false;
        // A hold ran out without the point being believed: the model is
        // followed until it is believed again, rather than held once more
        // (an arm out of the picture would otherwise stutter).
        bool given_up = false;
        double held_for = 0;
        uint32_t rejected = 0;      // pictures in a row it failed
        // The point relative to its parent, in the picture and in the world,
        // and how fast that is changing.
        std::array<float, 2> offset{}, offset_speed{};
        std::array<float, 3> world_offset{}, world_speed{};
        float bone = 0;             // its usual length (metres or shoulder widths)
        uint32_t bone_samples = 0;
    };
    static constexpr std::array<Limb, 8> limbs_{{
        {pose_point::elbow_left, pose_point::shoulder_left}, {pose_point::elbow_right, pose_point::shoulder_right},
        {pose_point::wrist_left, pose_point::elbow_left}, {pose_point::wrist_right, pose_point::elbow_right},
        {pose_point::knee_left, pose_point::hip_left}, {pose_point::knee_right, pose_point::hip_right},
        {pose_point::ankle_left, pose_point::knee_left}, {pose_point::ankle_right, pose_point::knee_right},
    }};
    bool enabled_;
    std::array<Memory, limbs_.size()> memory_{};
    uint32_t held_ = 0;
};

// In the menus the hands are a cursor, and the title moves it about 2600
// pixels for a metre of hand: a hand held still on a button still wanders a
// few millimetres between pictures, which is a cursor that will not settle,
// and a single wrong picture throws it across the screen. While racing the
// same hands are gestures, where any delay costs, so this only acts in the
// menus.
//
// Each hand is taken relative to the shoulders (the body may sway; the
// cursor follows the hand on the body), and:
//   - inside a small circle around where the cursor rests, nothing moves;
//   - beyond it, the rest point is dragged along behind the hand, so a slow
//     movement starts at once and lags by no more than that circle;
//   - a fast movement is followed exactly, with no lag at all;
//   - a single picture farther than a hand moves is ignored, unless the next
//     one agrees with it.
// The skeleton still carries the hand's depth and the rest of the body as
// measured, so pushing towards a button is not held back.
class MenuHandSteadying {
public:
    // Steadies unless the player's switch is off (menu_steadying below).
    static MenuHandSteadying from_environment();
    explicit MenuHandSteadying(bool enabled = true) : enabled_(enabled) {}

    // Steadies the two hands (and their wrists) in place when in_menu;
    // racing passes them through and starts again on the next menu.
    void steady(SkeletonJoints& joints, bool in_menu);
    void forget();
    // One hand (0 left, 1 right) starts again, as when the sensor lost it.
    void forget_hand(size_t side);

    // The circle's radius is the player's (menu_steady_radius below); a step
    // beyond follow_radius, or twice the circle if that is wider, is
    // followed exactly.
    static constexpr float default_rest_radius = 0.008f;  // metres: about 20 cursor pixels
    static constexpr float max_rest_radius = 0.03f;
    static constexpr float follow_radius = 0.03f;
    static constexpr float max_jump = 0.25f;      // metres in one picture
    static constexpr uint32_t max_ignored_in_a_row = 1;

private:
    struct Hand {
        bool known = false;
        std::array<float, 2> rest{}, last{};  // x and y relative to the shoulders
        uint32_t ignored = 0;
    };
    bool enabled_;
    std::array<Hand, 2> hands_{};
};

// The player's switch for the menu steadying, for every MenuHandSteadying at
// once (webcam and Kinect): on unless SFR_MENU_HAND_STEADY=0 (the
// launcher's circle set to 0) turns it off.
void set_menu_steadying(bool on);
bool menu_steadying();

// The circle a still hand stays in, in metres: SFR_MENU_STEADY_RADIUS in
// millimetres (1 to 30, the launcher's setting), else the default.
float menu_steady_radius();

}
