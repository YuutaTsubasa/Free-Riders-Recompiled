#include "touch_labels.h"
#include "touch_controls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace sfr {
namespace {
using Point = std::pair<float, float>;
using Polyline = std::initializer_list<Point>;

struct Glyph {
    char letter;
    float width;
    std::initializer_list<Polyline> lines;
};

// Capitals one unit high, y down, drawn as connected straight strokes. The
// bowls of B, R and S are cut corners: at a button's size, with round stroke
// ends, they read as curves.
const Glyph glyphs[] = {
    {'A', 0.62f, {{{0.0f, 1.0f}, {0.31f, 0.0f}, {0.62f, 1.0f}}, {{0.12f, 0.64f}, {0.5f, 0.64f}}}},
    {'B', 0.6f,
     {{{0.0f, 0.48f}, {0.4f, 0.48f}, {0.53f, 0.37f}, {0.53f, 0.11f}, {0.41f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.0f},
       {0.44f, 1.0f}, {0.6f, 0.87f}, {0.6f, 0.61f}, {0.44f, 0.48f}}}},
    {'L', 0.5f, {{{0.0f, 0.0f}, {0.0f, 1.0f}, {0.5f, 1.0f}}}},
    {'R', 0.6f,
     {{{0.0f, 1.0f}, {0.0f, 0.0f}, {0.43f, 0.0f}, {0.58f, 0.12f}, {0.58f, 0.39f}, {0.43f, 0.51f}, {0.0f, 0.51f}},
      {{0.3f, 0.51f}, {0.6f, 1.0f}}}},
    {'S', 0.6f,
     {{{0.58f, 0.13f}, {0.45f, 0.0f}, {0.15f, 0.0f}, {0.02f, 0.12f}, {0.02f, 0.35f}, {0.14f, 0.46f}, {0.46f, 0.54f},
       {0.6f, 0.65f}, {0.6f, 0.88f}, {0.47f, 1.0f}, {0.14f, 1.0f}, {0.0f, 0.87f}}}},
    {'T', 0.62f, {{{0.0f, 0.0f}, {0.62f, 0.0f}}, {{0.31f, 0.0f}, {0.31f, 1.0f}}}},
    {'X', 0.62f, {{{0.0f, 0.0f}, {0.62f, 1.0f}}, {{0.62f, 0.0f}, {0.0f, 1.0f}}}},
    {'Y', 0.62f, {{{0.0f, 0.0f}, {0.31f, 0.5f}, {0.62f, 0.0f}}, {{0.31f, 0.5f}, {0.31f, 1.0f}}}},
    // The frame-rate readout's (fps_readout.h): "F", "P" and digits of one width.
    {'F', 0.5f, {{{0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.0f}}, {{0.0f, 0.48f}, {0.4f, 0.48f}}}},
    {'P', 0.58f,
     {{{0.0f, 1.0f}, {0.0f, 0.0f}, {0.43f, 0.0f}, {0.58f, 0.12f}, {0.58f, 0.4f}, {0.43f, 0.52f}, {0.0f, 0.52f}}}},
    {'0', 0.55f,
     {{{0.12f, 0.0f}, {0.43f, 0.0f}, {0.55f, 0.12f}, {0.55f, 0.88f}, {0.43f, 1.0f}, {0.12f, 1.0f}, {0.0f, 0.88f},
       {0.0f, 0.12f}, {0.12f, 0.0f}}}},
    {'1', 0.55f, {{{0.08f, 0.2f}, {0.3f, 0.0f}, {0.3f, 1.0f}}, {{0.08f, 1.0f}, {0.52f, 1.0f}}}},
    {'2', 0.55f,
     {{{0.0f, 0.15f}, {0.12f, 0.0f}, {0.43f, 0.0f}, {0.55f, 0.12f}, {0.55f, 0.38f}, {0.0f, 1.0f}, {0.55f, 1.0f}}}},
    {'3', 0.55f,
     {{{0.0f, 0.1f}, {0.1f, 0.0f}, {0.45f, 0.0f}, {0.55f, 0.1f}, {0.55f, 0.38f}, {0.45f, 0.48f}, {0.15f, 0.48f}},
      {{0.45f, 0.48f}, {0.55f, 0.58f}, {0.55f, 0.9f}, {0.45f, 1.0f}, {0.1f, 1.0f}, {0.0f, 0.9f}}}},
    {'4', 0.55f, {{{0.42f, 1.0f}, {0.42f, 0.0f}, {0.0f, 0.68f}, {0.55f, 0.68f}}}},
    {'5', 0.55f,
     {{{0.55f, 0.0f}, {0.05f, 0.0f}, {0.02f, 0.45f}, {0.42f, 0.42f}, {0.55f, 0.54f}, {0.55f, 0.88f}, {0.43f, 1.0f},
       {0.1f, 1.0f}, {0.0f, 0.9f}}}},
    {'6', 0.55f,
     {{{0.5f, 0.05f}, {0.4f, 0.0f}, {0.15f, 0.0f}, {0.0f, 0.15f}, {0.0f, 0.88f}, {0.12f, 1.0f}, {0.43f, 1.0f},
       {0.55f, 0.88f}, {0.55f, 0.6f}, {0.43f, 0.48f}, {0.12f, 0.48f}, {0.0f, 0.58f}}}},
    {'7', 0.55f, {{{0.0f, 0.0f}, {0.55f, 0.0f}, {0.2f, 1.0f}}}},
    {'8', 0.55f,
     {{{0.12f, 0.48f}, {0.03f, 0.38f}, {0.03f, 0.1f}, {0.13f, 0.0f}, {0.42f, 0.0f}, {0.52f, 0.1f}, {0.52f, 0.38f},
       {0.43f, 0.48f}, {0.12f, 0.48f}, {0.0f, 0.6f}, {0.0f, 0.88f}, {0.12f, 1.0f}, {0.43f, 1.0f}, {0.55f, 0.88f},
       {0.55f, 0.6f}, {0.43f, 0.48f}}}},
    {'9', 0.55f,
     {{{0.05f, 0.95f}, {0.15f, 1.0f}, {0.4f, 1.0f}, {0.55f, 0.85f}, {0.55f, 0.12f}, {0.43f, 0.0f}, {0.12f, 0.0f},
       {0.0f, 0.12f}, {0.0f, 0.4f}, {0.12f, 0.52f}, {0.43f, 0.52f}, {0.55f, 0.42f}}}},
};

const Glyph* find_glyph(char letter) {
    for (const Glyph& glyph : glyphs)
        if (glyph.letter == letter) return &glyph;
    return nullptr;
}

// Stroke weight and letter spacing, in cap heights.
constexpr float weight = 0.18f;
constexpr float spacing = 0.3f;

struct LabelSpec {
    std::string_view text;
    float centre_y;    // of the cap height, in button radii from the centre
    float cap_height;  // in button radii
    float condense;    // horizontal scale of the letters
    bool on_knob;
};

LabelSpec spec(size_t circle) {
    switch (circle) {
    // The sticks' names ride on their knobs (radius 0.45 of the circle's).
    case TouchControls::stick: return {"LS", 0.0f, 0.3f, 1.0f, true};
    case TouchControls::right_stick: return {"RS", 0.0f, 0.32f, 1.0f, true};
    case TouchControls::a:
    case TouchControls::b:
    case TouchControls::x:
    case TouchControls::y: {
        static constexpr std::string_view names[] = {"A", "B", "X", "Y"};
        return {names[circle - TouchControls::a], 0.0f, 0.76f, 1.0f, false};
    }
    case TouchControls::trigger: return {"RT", 0.0f, 0.56f, 1.0f, false};
    case TouchControls::lt: return {"LT", 0.0f, 0.56f, 1.0f, false};
    case TouchControls::start: return {"START", 0.0f, 0.4f, 0.78f, false};
    // Above the punch's arrow (blit.hlsl draws it in the lower half).
    case TouchControls::lb: return {"LB", -0.32f, 0.44f, 1.0f, false};
    case TouchControls::rb: return {"RB", -0.32f, 0.44f, 1.0f, false};
    }
    return {};
}

float segment_distance(const LabelStroke& s, float x, float y) {
    const float bx = s.x1 - s.x0, by = s.y1 - s.y0, px = x - s.x0, py = y - s.y0;
    const float length = bx * bx + by * by;
    const float h = length > 0.0f ? std::clamp((px * bx + py * by) / length, 0.0f, 1.0f) : 0.0f;
    return std::hypot(px - bx * h, py - by * h);
}
}

std::vector<LabelStroke> glyph_strokes(char letter) {
    std::vector<LabelStroke> strokes;
    if (const Glyph* glyph = find_glyph(letter))
        for (const Polyline& line : glyph->lines)
            for (auto point = line.begin(); point + 1 < line.end(); ++point)
                strokes.push_back({point->first, point->second, (point + 1)->first, (point + 1)->second});
    return strokes;
}

float glyph_width(char letter) {
    const Glyph* glyph = find_glyph(letter);
    return glyph ? glyph->width : 0.0f;
}

TouchLabel touch_label(size_t circle) {
    const LabelSpec s = spec(circle);
    TouchLabel label;
    label.text = s.text;
    label.on_knob = s.on_knob;
    label.cap_height = s.cap_height;
    if (s.text.empty()) return label;
    const float height = s.cap_height, condense = s.condense;
    float width = 0.0f;
    for (char letter : s.text) width += glyph_width(letter) * condense;
    width += spacing * float(s.text.size() - 1);
    // Centred on the stroke centres: each glyph's outline spans 0..width.
    float x = -width * height / 2.0f;
    const float top = s.centre_y - height / 2.0f;
    for (char letter : s.text) {
        for (LabelStroke stroke : glyph_strokes(letter)) {
            label.strokes.push_back({x + stroke.x0 * condense * height, top + stroke.y0 * height,
                                     x + stroke.x1 * condense * height, top + stroke.y1 * height});
        }
        x += (glyph_width(letter) * condense + spacing) * height;
    }
    label.half_width = weight * height / 2.0f;
    return label;
}

float touch_label_distance(const TouchLabel& label, float x, float y) {
    float distance = 1.0e4f;
    for (const LabelStroke& stroke : label.strokes) distance = std::min(distance, segment_distance(stroke, x, y));
    return distance - label.half_width;
}

namespace {
constexpr std::string_view readout_suffix_text = "FPS";

// A glyph's strokes moved right by x (in cap heights).
void place(std::vector<LabelStroke>& strokes, char letter, float x) {
    for (LabelStroke s : glyph_strokes(letter)) strokes.push_back({s.x0 + x, s.y0, s.x1 + x, s.y1});
}

std::vector<LabelStroke> readout_suffix() {
    std::vector<LabelStroke> strokes;
    float x = 0.0f;
    for (char letter : readout_suffix_text) {
        place(strokes, letter, x);
        x += glyph_width(letter) + spacing;
    }
    return strokes;
}

void append_strokes(std::string& text, const std::vector<LabelStroke>& strokes, const char* indent) {
    char line[160];
    for (const LabelStroke& s : strokes) {
        std::snprintf(line, sizeof line, "%sd = min(d, label_stroke(p, float2(%.4f, %.4f), float2(%.4f, %.4f)));\n",
                      indent, s.x0, s.y0, s.x1, s.y1);
        text += line;
    }
}
}

size_t readout_digits(uint32_t value) { return std::to_string(std::min(value, readout_max)).size(); }

float readout_suffix_width() {
    float width = 0.0f;
    for (char letter : readout_suffix_text) width += glyph_width(letter);
    return width + spacing * float(readout_suffix_text.size() - 1);
}

float readout_width(uint32_t value) {
    return float(readout_digits(value)) * readout_advance + readout_space + readout_suffix_width();
}

float readout_half_width() { return weight / 2.0f; }

std::vector<LabelStroke> readout_strokes(uint32_t value) {
    const std::string digits = std::to_string(std::min(value, readout_max));
    std::vector<LabelStroke> strokes;
    for (size_t k = 0; k < digits.size(); ++k) place(strokes, digits[k], float(k) * readout_advance);
    const float x = float(digits.size()) * readout_advance + readout_space;
    for (LabelStroke s : readout_suffix()) strokes.push_back({s.x0 + x, s.y0, s.x1 + x, s.y1});
    return strokes;
}

float readout_distance(uint32_t value, float x, float y) {
    float distance = 1.0e4f;
    for (const LabelStroke& stroke : readout_strokes(value)) distance = std::min(distance, segment_distance(stroke, x, y));
    return distance - readout_half_width();
}

std::string touch_labels_hlsl() {
    std::string text =
        "// Generated by tests/touch_labels_test.cpp --write from src/touch_labels.cpp: do not edit.\n"
        "// The on-screen buttons' names as round-ended strokes (touch_labels.h), in\n"
        "// button radii from the circle's centre (or the knob's), y down.\n"
        "\n"
        "float label_stroke(float2 p, float2 a, float2 b) {\n"
        "    const float2 pa = p - a, ba = b - a;\n"
        "    return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba)));\n"
        "}\n"
        "\n"
        "// Whether circle i's name rides on its stick's knob.\n"
        "bool label_on_knob(int i) { return ";
    bool first = true;
    for (size_t i = 0; i < TouchOverlay::circles; ++i)
        if (touch_label(i).on_knob) {
            text += (first ? "" : " || ") + std::string("i == ") + std::to_string(i);
            first = false;
        }
    text += first ? "false; }\n" : "; }\n";
    text +=
        "\n"
        "// Distance from p to circle i's name (negative inside a stroke); i is a\n"
        "// constant in the unrolled loop, so only its own strokes remain.\n"
        "float label_distance(int i, float2 p) {\n"
        "    float d = 1.0e4;\n";
    char line[512];
    for (size_t i = 0; i < TouchOverlay::circles; ++i) {
        const TouchLabel label = touch_label(i);
        if (label.strokes.empty()) continue;
        std::snprintf(line, sizeof line, "    if (i == %zu) {  // %.*s\n", i, int(label.text.size()), label.text.data());
        text += line;
        for (const LabelStroke& s : label.strokes) {
            std::snprintf(line, sizeof line, "        d = min(d, label_stroke(p, float2(%.4f, %.4f), float2(%.4f, %.4f)));\n",
                          s.x0, s.y0, s.x1, s.y1);
            text += line;
        }
        std::snprintf(line, sizeof line, "        return d - %.4f;\n    }\n", label.half_width);
        text += line;
    }
    text += "    return d;\n}\n";
    std::snprintf(line, sizeof line,
                  "\n"
                  "// The frame-rate readout (fps_readout.h), in cap heights from its upper left\n"
                  "// corner: digits every g_ReadoutAdvance, then a space and \"FPS\".\n"
                  "static const float g_ReadoutAdvance = %.4f;\n"
                  "static const float g_ReadoutSpace = %.4f;\n"
                  "static const float g_ReadoutSuffixWidth = %.4f;\n",
                  readout_advance, readout_space, readout_suffix_width());
    text += line;
    std::snprintf(line, sizeof line, "static const uint g_ReadoutMax = %u;\n", unsigned(readout_max));
    text += line;
    text +=
        "\n"
        "// Distance from p to a digit (negative inside).\n"
        "float readout_digit(uint digit, float2 p) {\n"
        "    float d = 1.0e4;\n";
    for (char digit = '0'; digit <= '9'; ++digit) {
        std::snprintf(line, sizeof line, "    %sif (digit == %c) {\n", digit == '0' ? "" : "else ", digit);
        text += line;
        append_strokes(text, glyph_strokes(digit), "        ");
        text += "    }\n";
    }
    std::snprintf(line, sizeof line, "    return d - %.4f;\n}\n", readout_half_width());
    text += line;
    text +=
        "\n"
        "// Distance from p to \"FPS\".\n"
        "float readout_suffix(float2 p) {\n"
        "    float d = 1.0e4;\n";
    append_strokes(text, readout_suffix(), "    ");
    std::snprintf(line, sizeof line, "    return d - %.4f;\n}\n", readout_half_width());
    text += line;
    return text;
}
}
