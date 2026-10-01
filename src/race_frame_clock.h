#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace sfr {
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
        if (first) return RaceFrameStep{1.0 / 60.0, 1.0f, false};

        // A stopped application must not simulate seconds in one collision
        // update. Report the omitted time so a test cannot hide that drift.
        // Keep the lower bound positive: zero selects the title's unrelated
        // integer-quantized fallback when used as an original forced step.
        const uint64_t bounded_ns = std::clamp<uint64_t>(elapsed_ns, 1'000, 250'000'000);
        return RaceFrameStep{double(elapsed_ns) * 1e-9,
                             float(double(bounded_ns) * 60e-9),
                             bounded_ns != elapsed_ns};
    }
private:
    uint64_t scene_ = 0;
    uint64_t previous_ns_ = 0;
};
}
