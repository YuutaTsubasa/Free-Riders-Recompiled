#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
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
