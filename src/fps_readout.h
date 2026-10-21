#pragma once
#include <cstdint>

namespace sfr {
// The in-game frame-rate counter (Issue #236; launcher "Show FPS",
// SFR_FPS_OVERLAY=1): the presented frames per second in the image's upper
// left corner, drawn by the blit with the touch buttons' strokes
// (touch_labels.h), so phones, which have no window title, show it too.

// Counts presented frames and gives their rate, taken every half second.
class FpsCounter {
public:
    static constexpr double period = 0.5;  // seconds between new values
    // A frame presented at `seconds` (any steady clock).
    void presented(double seconds);
    // Frames per second over the last full period, rounded; 0 until one has
    // passed.
    uint32_t value() const { return value_; }

private:
    double start_ = 0.0;
    bool started_ = false;
    uint32_t frames_ = 0;
    uint32_t value_ = 0;
};

// SFR_FPS_OVERLAY=1.
bool fps_overlay_enabled();

// The blit's word for the readout (blit.hlsl): 0 hides it, otherwise bit 16
// set and the value (at most 999) in the low bits.
uint32_t fps_readout_word(bool shown, uint32_t value);
}
