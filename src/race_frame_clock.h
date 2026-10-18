#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace sfr {
template<class Update>
void for_each_race_ui_step(float frames, Update&& update) {
    // A guest UI update evaluates this frame's actions and advances at most
    // one timeline frame. Replay intermediate updates instead of skipping
    // their actions. The callback stops on the original completion signal.
    if (!std::isfinite(frames) || frames<=0) frames=1;
    frames=(std::min)(frames,15.0f);
    while(frames>0) {
        const float step=(std::min)(frames,1.0f);
        if(!update(step)) break;
        frames-=step;
    }
}

struct RaceFrameStep {
    double elapsed_seconds = 0;
    float frames = 1;
    bool clamped = false;
};

// Pure clock policy, independent of the guest address space and host clock.
class RaceFrameClock {
public:
    std::optional<RaceFrameStep> update(uint64_t now_ns, uint64_t scene) {
        if (!scene) {
            scene_ = 0;
            return std::nullopt;
        }
        const bool first = scene != scene_ || now_ns < previous_ns_;
        const uint64_t elapsed_ns = first ? 0 : now_ns - previous_ns_;
        scene_ = scene;
        previous_ns_ = now_ns;
        if (first) {
            carry_ = 0;
            return RaceFrameStep{1.0 / 60.0, 1.0f, false};
        }

        // A stopped application must not simulate seconds in one collision
        // update. Report the omitted time so a test cannot hide that drift.
        // Keep the lower bound positive: zero selects the title's unrelated
        // integer-quantized fallback when used as an original forced step.
        const uint64_t bounded_ns = std::clamp<uint64_t>(elapsed_ns, 1'000, 250'000'000);
        // A step within a tenth of one original frame is exactly one frame,
        // the rest carried into the next steps (so elapsed time is kept).
        // At the 60 fps the title was made for, the race then steps exactly
        // as on the console: some of its physics scale an impulse by the
        // step, and a takeoff on a frame a little longer than 1/60 s threw
        // a charged jump off the side of Rocky Ridge.
        const double wanted = double(bounded_ns) * 60e-9 + carry_;
        const double frames = std::fabs(wanted - 1.0) <= 0.1 ? 1.0 : wanted;
        carry_ = wanted - frames;
        return RaceFrameStep{double(elapsed_ns) * 1e-9, float(frames), bounded_ns != elapsed_ns};
    }
private:
    uint64_t scene_ = 0;
    uint64_t previous_ns_ = 0;
    double carry_ = 0;  // time owed to (or ahead of) the steps given so far, in frames
};

// Frame skipping for a race that cannot keep 60 fps (SFR_RACE_FRAMESKIP).
// The race keeps the console's step of one sixtieth of a second, which its
// collisions were made for: a longer step let a fast racer through the side
// of a ramp. When a frame falls behind real time by more than a frame, the
// next one runs the race without drawing it, so the race keeps its speed and
// the screen updates less often. An undrawn frame also leaves out most of
// the game's own drawing work (race_frame_clock_hooks.cpp, undrawn_skips),
// so it is far shorter than a drawn one. At most max_skips frames in a row
// go undrawn (four: the screen still shows at least 12 frames a second); a
// race still behind after them (the game itself, not the drawing, too slow)
// gives the time up and slows down, as on the console.
class RaceFrameSkip {
public:
    static constexpr uint64_t frame_ns = 16'666'667;
    // skip_slow (SFR_TEST_RACE_SKIP_SLOW, an experiment): skip also when
    // undrawn frames are a frame or longer, if they are under three quarters
    // of a drawn one. The race then cannot keep real time, but comes closer
    // to it than by drawing every frame (Issue #176: a Pixel 10a's undrawn
    // frames took ~17 ms in a race's busiest parts, its drawn ones ~35).
    explicit RaceFrameSkip(uint32_t max_skips = 4, bool skip_slow = false)
        : max_skips_(max_skips), skip_slow_(skip_slow) {}
    struct Decision {
        bool draw = true;      // draw the next frame
        uint64_t wait_ns = 0;  // wait this long first (the frame ended early)
    };
    // Called at the end of each frame. The race paces itself by this schedule
    // (it replaces the frame-rate cap while racing): an early frame waits for
    // its time, a late one does not, and undrawn frames, being shorter, let
    // the race catch up. A separate cap that also held undrawn frames to a
    // sixtieth of a second kept the race behind for good, and it skipped most
    // frames even on a phone that draws 60 (an AYN Thor).
    Decision next(uint64_t now_ns, bool racing) {
        if (!racing) {
            started_ = false;
            skipped_ = 0;
            return {};
        }
        if (!started_) {
            started_ = true;
            due_ns_ = now_ns + frame_ns;
            skipped_ = 0;
            last_ns_ = now_ns;
            undrawn_ns_ = 0;
            drawn_ns_ = 0;
            frames_ = 0;
            return {};
        }
        // How long the frame that just ended took, undrawn and drawn apart: a
        // slow average of each, each frame counted as at most two, so a hitch now and
        // then (a load, a shader) does not stop the skipping (on an AYN Thor
        // it held skips back 204 times in a race whose undrawn frames
        // averaged 6 ms).
        if (now_ns > last_ns_) {
            const uint64_t took = (std::min)(now_ns - last_ns_, 2 * frame_ns);
            uint64_t& average = skipped_ ? undrawn_ns_ : drawn_ns_;
            average = average ? (average * 7 + took) / 8 : took;
        }
        last_ns_ = now_ns;
        ++frames_;
        if (now_ns < due_ns_) {  // on time: wait for the frame's slot
            const uint64_t wait = due_ns_ - now_ns;
            due_ns_ += frame_ns;
            skipped_ = 0;
            last_ns_ = now_ns + wait;
            return {true, wait};
        }
        // How late the frame that just ended is, against a clock that moves
        // on a sixtieth of a second a frame.
        const uint64_t late = now_ns - due_ns_;
        due_ns_ += frame_ns;
        // Far behind (a load, a stall, or a game too slow to catch up even
        // undrawn): start again from now. The race then runs slower than
        // real time, as the console's did when it could not keep up: its
        // step stays the console's, so it never plays differently.
        if (late > 6 * frame_ns) {
            due_ns_ = now_ns + frame_ns;
            skipped_ = 0;
            ++resyncs_;
            return {};
        }
        // A frame more than a whole frame late: a phone that keeps 60 is
        // late by less now and then, and should not drop those frames.
        // Not when undrawn frames are no shorter than a frame (the game
        // itself too slow): they could not catch up, and the screen would
        // only show fewer frames. Tried again once a second.
        const bool helps = undrawn_ns_ < frame_ns || frames_ % 60 == 0 ||
                           (skip_slow_ && undrawn_ns_ * 4 < drawn_ns_ * 3);
        if (late > frame_ns && skipped_ < max_skips_ && !helps) ++blocked_;
        if (late > frame_ns && skipped_ < max_skips_ && helps) {
            ++skipped_;
            return {false, 0};
        }
        skipped_ = 0;
        return {};
    }
private:
    uint32_t max_skips_;
    bool skip_slow_;
    bool started_ = false;
    uint64_t due_ns_ = 0;   // when the frame being run should end
    uint32_t skipped_ = 0;  // undrawn frames in a row
    uint64_t last_ns_ = 0;     // when the frame that just ended began
    uint64_t undrawn_ns_ = 0;  // how long an undrawn frame takes (average)
    uint64_t drawn_ns_ = 0;    // and a drawn one
    uint64_t frames_ = 0;
    uint64_t resyncs_ = 0, blocked_ = 0;
public:
    // For the log: times the race started its count again, skips held back
    // because undrawn frames were too slow, and an undrawn frame's average.
    uint64_t resyncs() const { return resyncs_; }
    uint64_t blocked() const { return blocked_; }
    uint64_t undrawn_ns() const { return undrawn_ns_; }
    uint64_t drawn_ns() const { return drawn_ns_; }
};

// SFR_RACE_FRAMESKIP=1|0: frame skipping on or off. On by default: a race
// keeps real time wherever drawing cannot keep 60 fps (a phone, a handheld
// PC, a slow GPU), and on Android it also keeps the race's step fixed
// (race_frame_clock_hooks.cpp). A device that keeps 60 draws every frame.
inline bool race_frameskip_enabled() {
    static const bool enabled = [] {
        if (const char* value = std::getenv("SFR_RACE_FRAMESKIP")) return *value && *value != '0';
        return true;
    }();
    return enabled;
}

// SFR_PRESENT_LIMIT_RACING=N (guest_graphics_hooks.cpp): the run ends at the
// Nth present with the title's race flag set, so the menus and the loading
// screen, however long they take on a PC, are not counted. A limit counted
// from the last SFR_SAY word includes the loading screen, which presents
// hundreds of times a second on a fast PC and a few dozen on a slow one.
class RacePresentLimit {
public:
    explicit RacePresentLimit(uint32_t limit) : limit_(limit) {}
    // Counts this present; true once the limit is reached (never with 0).
    bool reached(bool racing) {
        if (!limit_) return false;
        if (racing) ++counted_;
        return counted_ >= limit_;
    }
    uint32_t counted() const { return counted_; }
private:
    uint32_t limit_;
    uint32_t counted_ = 0;
};
}
