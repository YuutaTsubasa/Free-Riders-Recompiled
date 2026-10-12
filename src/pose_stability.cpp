#include "pose_stability.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

namespace sfr {

PoseStabilizer PoseStabilizer::from_environment() {
    const char* const text = std::getenv("SFR_POSE_STABILIZE");
    return PoseStabilizer(!(text && *text == '0'));
}

void PoseStabilizer::forget() {
    for (auto& memory : memory_) {
        const float bone = memory.bone;
        const uint32_t samples = memory.bone_samples;
        memory = {};
        // The player's bones stay the player's: only where they were is lost.
        memory.bone = bone;
        memory.bone_samples = samples;
    }
    held_ = 0;
}

void PoseStabilizer::stabilize(PoseLandmarks& landmarks, double interval) {
    held_ = 0;
    if (!enabled_) return;
    if (!std::isfinite(interval) || interval <= 0 || interval > 0.5) {
        forget();
        if (!std::isfinite(interval) || interval <= 0) return;
    }
    using namespace pose_point;
    const bool world = landmarks[shoulder_left].has_world;
    // Picture models are measured in shoulder widths, so that a player near
    // the camera and one far from it move the same distance.
    const float span = std::hypot(landmarks[shoulder_left].x - landmarks[shoulder_right].x,
                                  landmarks[shoulder_left].y - landmarks[shoulder_right].y);
    if (!world && !(span >= 1.0f)) return;
    const auto offset_of = [&](uint32_t point, uint32_t parent) {
        return std::array<float, 2>{landmarks[point].x - landmarks[parent].x, landmarks[point].y - landmarks[parent].y};
    };
    const auto world_offset_of = [&](uint32_t point, uint32_t parent) {
        const auto& a = landmarks[point].world;
        const auto& b = landmarks[parent].world;
        return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    // A length in the units this model is checked in.
    const auto length = [&](const std::array<float, 2>& offset, const std::array<float, 3>& world_offset) {
        return world ? std::hypot(world_offset[0], world_offset[1], world_offset[2])
                     : std::hypot(offset[0], offset[1]) / span;
    };

    // Parents come before their children (the elbows before the wrists),
    // so a held elbow is where its wrist is measured from.
    for (size_t i = 0; i < limbs_.size(); ++i) {
        const Limb limb = limbs_[i];
        Memory& memory = memory_[i];
        PoseLandmark& point = landmarks[limb.point];
        const auto offset = offset_of(limb.point, limb.parent);
        const auto world_offset = world ? world_offset_of(limb.point, limb.parent) : std::array<float, 3>{};
        const float bone = length(offset, world_offset);

        bool believed = std::isfinite(bone);
        if (world && !(point.score >= min_score)) believed = false;
        // A bone checked only once it has a usual length to be checked against.
        if (believed && memory.bone_samples >= 5 && bone > memory.bone * max_stretch) believed = false;
        // A point given up on was following the model's guesses: where it
        // comes back from says nothing, so its return is not a jump.
        if (believed && memory.known && !memory.given_up) {
            // How far the point moved against its parent, beyond what the
            // offset's own speed would have carried it.
            const std::array<float, 2> moved{offset[0] - memory.offset[0], offset[1] - memory.offset[1]};
            const std::array<float, 3> world_moved{world_offset[0] - memory.world_offset[0],
                                                   world_offset[1] - memory.world_offset[1],
                                                   world_offset[2] - memory.world_offset[2]};
            const float jump = length(moved, world_moved);
            if (jump > (world ? max_jump_metres : max_jump_spans) && memory.rejected < max_rejected_in_a_row)
                believed = false;
        }

        if (!believed && memory.known && !memory.given_up && memory.held_for + interval <= max_hold_seconds) {
            // Held where it was against its parent, carried on by its speed,
            // which dies away (the distance a decaying speed covers).
            memory.held_for += interval;
            ++memory.rejected;
            memory.holding = true;
            const float carried = float(speed_decay_seconds * (1.0 - std::exp(-interval / speed_decay_seconds)));
            const float slowing = float(std::exp(-interval / speed_decay_seconds));
            for (int axis = 0; axis < 2; ++axis) {
                memory.offset[axis] += memory.offset_speed[axis] * carried;
                memory.offset_speed[axis] *= slowing;
            }
            for (int axis = 0; axis < 3; ++axis) {
                memory.world_offset[axis] += memory.world_speed[axis] * carried;
                memory.world_speed[axis] *= slowing;
            }
            point.x = landmarks[limb.parent].x + memory.offset[0];
            point.y = landmarks[limb.parent].y + memory.offset[1];
            if (world)
                for (int axis = 0; axis < 3; ++axis) point.world[axis] = landmarks[limb.parent].world[axis] + memory.world_offset[axis];
            ++held_;
            continue;
        }

        // Believed, or held as long as it may be: the model's point it is.
        // Its speed comes from two believed points: not across a give-up,
        // whose jump would be carried by the next hold.
        if (memory.known && !memory.holding && !memory.given_up) {
            for (int axis = 0; axis < 2; ++axis)
                memory.offset_speed[axis] = float((offset[axis] - memory.offset[axis]) / interval);
            for (int axis = 0; axis < 3; ++axis)
                memory.world_speed[axis] = float((world_offset[axis] - memory.world_offset[axis]) / interval);
        } else {
            memory.offset_speed = {};
            memory.world_speed = {};
        }
        memory.offset = offset;
        memory.world_offset = world_offset;
        memory.known = std::isfinite(bone);
        memory.given_up = !believed && memory.known;
        memory.holding = false;
        memory.held_for = 0;
        memory.rejected = 0;
        // The usual length learns from points the model is sure of; the
        // first few set it, then it moves slowly, so a run of wrong points
        // cannot teach it a wrong arm.
        const bool sure = (!world || point.score >= 0.7f) && bone > 0;
        if (believed && sure) {
            if (memory.bone_samples < 5) {
                memory.bone = (memory.bone * float(memory.bone_samples) + bone) / float(memory.bone_samples + 1);
                ++memory.bone_samples;
            } else if (bone < memory.bone * max_stretch) {
                memory.bone += 0.05f * (bone - memory.bone);
            }
        } else if (!believed && sure && memory.bone_samples >= 5) {
            // Held as long as it may be and still not believed, though the
            // model is sure: the usual length may be the wrong one (learned
            // from a player half in the picture, or from someone else), so
            // it gives way to what keeps being measured.
            memory.bone += 0.2f * (bone - memory.bone);
        }
    }
}

namespace {
std::atomic<int> menu_switch{-1};  // -1: not yet read from the environment
}

void set_menu_steadying(bool on) { menu_switch.store(on ? 1 : 0, std::memory_order_relaxed); }

bool menu_steadying() {
    int on = menu_switch.load(std::memory_order_relaxed);
    if (on < 0) {
        const char* const text = std::getenv("SFR_MENU_HAND_STEADY");
        int fresh = !(text && *text == '0');
        // A flip that came first wins over the environment.
        menu_switch.compare_exchange_strong(on, fresh, std::memory_order_relaxed);
        on = menu_switch.load(std::memory_order_relaxed);
    }
    return on != 0;
}

float menu_steady_radius() {
    static const float radius = [] {
        const char* const text = std::getenv("SFR_MENU_STEADY_RADIUS");
        if (!text || !*text) return MenuHandSteadying::default_rest_radius;
        char* end = nullptr;
        const double millimetres = std::strtod(text, &end);
        if (end == text || !std::isfinite(millimetres) || millimetres < 1) return MenuHandSteadying::default_rest_radius;
        return (std::min)(float(millimetres / 1000), MenuHandSteadying::max_rest_radius);
    }();
    return radius;
}

MenuHandSteadying MenuHandSteadying::from_environment() { return MenuHandSteadying(true); }

void MenuHandSteadying::forget() { hands_ = {}; }
void MenuHandSteadying::forget_hand(size_t side) { if (side < hands_.size()) hands_[side] = {}; }

void MenuHandSteadying::steady(SkeletonJoints& joints, bool in_menu) {
    if (!enabled_) return;
    if (!in_menu || !menu_steadying()) {
        forget();
        return;
    }
    namespace joint = nui_joint;
    const float rest_radius = menu_steady_radius();
    const float follow = (std::max)(follow_radius, 2 * rest_radius);
    const auto& shoulders = joints[joint::shoulder_center];
    const std::array<std::array<uint32_t, 2>, 2> pairs{{{joint::hand_left, joint::wrist_left},
                                                        {joint::hand_right, joint::wrist_right}}};
    for (size_t side = 0; side < pairs.size(); ++side) {
        Hand& hand = hands_[side];
        auto& point = joints[pairs[side][0]];
        const std::array<float, 2> now{point[0] - shoulders[0], point[1] - shoulders[1]};
        if (!std::isfinite(now[0]) || !std::isfinite(now[1])) {
            hand = {};
            continue;
        }
        if (!hand.known) {
            hand = {true, now, now, 0};
            continue;
        }
        // One wrong picture is ignored; two that agree are a movement.
        if (std::hypot(now[0] - hand.last[0], now[1] - hand.last[1]) > max_jump && hand.ignored < max_ignored_in_a_row) {
            ++hand.ignored;
        } else {
            hand.ignored = 0;
            hand.last = now;
            const float dx = now[0] - hand.rest[0], dy = now[1] - hand.rest[1];
            const float away = std::hypot(dx, dy);
            if (away > follow) hand.rest = now;
            else if (away > rest_radius) {
                const float pulled = 1.0f - rest_radius / away;
                hand.rest = {hand.rest[0] + dx * pulled, hand.rest[1] + dy * pulled};
            }
        }
        // Where the hand is drawn: the rest point on the shoulders as they
        // are now, the wrist moved with it.
        const float shift_x = shoulders[0] + hand.rest[0] - point[0], shift_y = shoulders[1] + hand.rest[1] - point[1];
        for (const uint32_t moved : pairs[side]) {
            joints[moved][0] += shift_x;
            joints[moved][1] += shift_y;
        }
    }
}

}
