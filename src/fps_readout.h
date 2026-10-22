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

// How far in from the image's corner the readout sits, in percent of the
// image's height (SFR_FPS_OVERLAY_INSET, at most fps_inset_max): a phone's
// rounded corners cut a readout drawn right in the corner (Issue #236).
constexpr uint32_t fps_inset_max = 20;
uint32_t fps_overlay_inset();

// The blit's word for the readout (blit.hlsl): 0 hides it, otherwise bit 16
// set, the value (at most 999) in the low bits and the inset (at most
// fps_inset_max) in bits 20 to 24. Bit 17 is not the readout's: it hides the
// touch buttons' names (touch_controls.h, touch_names_hidden_bit).
uint32_t fps_readout_word(bool shown, uint32_t value, uint32_t inset = 0);
}
