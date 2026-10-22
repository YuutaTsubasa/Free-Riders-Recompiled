#include "fps_readout.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sfr {
void FpsCounter::presented(double seconds) {
    if (!started_ || seconds < start_) {
        started_ = true;
        start_ = seconds;
        frames_ = 0;
        return;
    }
    ++frames_;
    const double elapsed = seconds - start_;
    if (elapsed < period) return;
    value_ = uint32_t(std::lround(double(frames_) / elapsed));
    start_ = seconds;
    frames_ = 0;
}

bool fps_overlay_enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("SFR_FPS_OVERLAY");
        return text && *text == '1';
    }();
    return enabled;
}

uint32_t fps_overlay_inset() {
    static const uint32_t inset = [] {
        const char* text = std::getenv("SFR_FPS_OVERLAY_INSET");
        return text ? std::min(uint32_t(std::strtoul(text, nullptr, 10)), fps_inset_max) : 0u;
    }();
    return inset;
}

uint32_t fps_readout_word(bool shown, uint32_t value, uint32_t inset) {
    return shown ? (1u << 16) | std::min(value, 999u) | (std::min(inset, fps_inset_max) << 20) : 0u;
}
}
