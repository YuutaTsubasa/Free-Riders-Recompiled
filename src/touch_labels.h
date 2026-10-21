#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {
// The names of the on-screen buttons (Issue #236): "A", "B", "X", "Y", "LB",
// "RT", "START", ... drawn by the blit (src/shaders/blit.hlsl) in each
// button's own colour, without a font: every letter is a few straight
// strokes with round ends. The blit cannot afford a texture or more push
// constants, so the strokes are baked into the shader as constants:
// src/shaders/touch_labels.hlsli is what touch_labels_hlsl() writes, and
// tests/touch_labels_test.cpp fails when the two differ (run it with
// --write to update the file).

// A straight stroke from (x0, y0) to (x1, y1). In a glyph the cap height is
// 1 (y down, 0 at the top); in a label the unit is the button's radius,
// centred on the button (or on a stick's knob), y down.
struct LabelStroke {
    float x0, y0, x1, y1;
};

// The letters the labels use; none for any other character.
std::vector<LabelStroke> glyph_strokes(char letter);
float glyph_width(char letter);  // in cap heights

struct TouchLabel {
    std::string_view text;
    std::vector<LabelStroke> strokes;
    float half_width = 0.0f;  // of each stroke, in button radii
    bool on_knob = false;     // centred on the stick's knob instead of the circle
    float cap_height = 0.0f;  // in button radii
};

// The label of a TouchControls::Button circle (touch_controls.h's order);
// an empty one for a circle without a name.
TouchLabel touch_label(size_t circle);
// Distance from (x, y) to the label's nearest stroke edge (negative inside),
// in button radii: what the blit evaluates.
float touch_label_distance(const TouchLabel& label, float x, float y);
// The frame-rate readout (fps_readout.h): up to three digits, a space and
// "FPS", in cap heights from its upper left corner, y down. The blit picks
// each digit's strokes itself; these give the same picture.
constexpr float readout_advance = 0.85f;  // from one digit to the next
constexpr float readout_space = 0.55f;    // between the number and "FPS"
constexpr uint32_t readout_max = 999;     // larger values show as this
size_t readout_digits(uint32_t value);
float readout_suffix_width();
float readout_width(uint32_t value);
float readout_half_width();
std::vector<LabelStroke> readout_strokes(uint32_t value);
float readout_distance(uint32_t value, float x, float y);

// The shader include with every label's strokes.
std::string touch_labels_hlsl();
}
