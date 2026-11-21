#pragma once
#include "touch_options.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sfr {
// Where each on-screen touch control sits and how big it is, when the player
// moved or resized it in the launcher's layout editor (Issue #443). The
// launcher keeps one list in settings.ini and hands it to the game as
// SFR_TOUCH_LAYOUT:
//
//   touch_layout=ls:l0.320,0.550,1.40;a:r0.276,0.820,0.80
//
// one entry a control, "name:" (touch_options::names) then the anchor and
// its offset, the centre's height and the size:
//
//   anchor  l, r or c: the control keeps its distance, in image heights,
//           from the left edge, from the right edge or from the centre, as
//           the default layout does from the nearer side edge (PR #444,
//           touch_anchor_x). The offset is that distance: inwards from l
//           and r, rightwards (negative leftwards) from c. The editor takes
//           whichever of the three is nearest, so a layout made on a 20:9
//           phone keeps each thumb's controls at its side, and START in the
//           middle, on a 16:9 tablet.
//   y       the centre's height, 0 at the top of the image, 1 at the bottom
//   size    a scale of the control's default radius, kept within
//           min_scale_of .. max_scale
//
// A control not listed keeps its default place and size: an empty list is
// the default layout, as before the editor. Unknown names and broken entries
// are ignored. Wherever a control is put, it stays whole on the image.
//
// Header only: the launcher uses it without the game's touch reader.
namespace touch_layout {
inline constexpr size_t count = touch_options::names.size();
// The width / height the default layout is drawn for.
inline constexpr float default_aspect = 16.0f / 9.0f;
// Smallest and largest size: half to twice the default, but never smaller
// than START's radius (a twentieth of the image's height, about 7 mm across
// on a phone) so a shrunk control can still be hit.
inline constexpr float min_scale = 0.5f, max_scale = 2.0f, min_radius = 0.05f;

// The default layout in a 16:9 image (0..1 across it, y down; radii in
// image heights), in circle order (TouchControls::Button).
inline constexpr std::array<std::array<float, 3>, count> defaults = [] {
    constexpr float aspect = default_aspect;
    constexpr float cx = 0.845f, cy = 0.70f, step = 0.12f, r = 0.075f;
    return std::array<std::array<float, 3>, count>{{
        {0.16f, 0.70f, 0.13f},            // LS, the stick
        {cx, cy + step, r},               // A
        {cx + step / aspect, cy, r},      // B
        {cx - step / aspect, cy, r},      // X
        {cx, cy - step, r},               // Y
        {0.66f, 0.84f, 0.065f},           // RT
        {0.5f, 0.08f, 0.05f},             // START
        // The race HUD keeps the screen's edges free between its corners
        // (Rings and items above, the place and lap below on the left; the
        // map above, the air gauge below on the right), so the punches sit
        // there, each on its own side.
        {0.065f, 0.44f, 0.07f},           // LB
        {0.935f, 0.44f, 0.07f},           // RB
        {0.34f, 0.86f, 0.055f},           // LT
        {0.70f, 0.46f, 0.09f},            // RS, the right stick
    }};
}();

// An x of the 16:9 layout in an image aspect times as wide as it is tall:
// the same distance in image heights from the nearer side edge (the centre
// stays the centre). touch_anchor_x.
inline float anchor_x(float x, float aspect) {
    if (!(aspect > 0.0f)) return x;
    const float k = default_aspect / aspect;
    if (x < 0.5f) return x * k;
    if (x > 0.5f) return 1.0f - (1.0f - x) * k;
    return x;
}

// How far outside its circle a control still reacts, in its radii.
inline constexpr float touch_slack = 1.25f;
// The share of the image's width the floating stick starts in, in a race.
inline constexpr float stick_side = 0.40f;

inline float min_scale_of(size_t circle) {
    return circle < count ? std::clamp(min_radius / defaults[circle][2], min_scale, 1.0f) : 1.0f;
}

// One control the player placed.
struct Place {
    char anchor = 'l';    // 'l', 'r' or 'c'
    float offset = 0.0f;  // image heights from that anchor (see above)
    float y = 0.0f;
    float scale = 1.0f;
    bool operator==(const Place&) const = default;
};

// Every control's place; none is the default.
struct Layout {
    std::array<std::optional<Place>, count> places{};
    bool empty() const {
        return std::none_of(places.begin(), places.end(), [](const auto& place) { return place.has_value(); });
    }
    bool operator==(const Layout&) const = default;
};

// A control's centre and radius (x 0..1 across an image of that aspect, y
// 0..1 down it, the radius in image heights): the default's unless placed,
// and kept whole on the image.
inline std::array<float, 3> resolve(size_t circle, const std::optional<Place>& place, float aspect) {
    if (circle >= count) return {};
    if (!(aspect > 0.0f)) aspect = default_aspect;
    const auto [x16, y16, r16] = defaults[circle];
    if (!place) return {anchor_x(x16, aspect), y16, r16};
    const float scale = std::isfinite(place->scale) ? std::clamp(place->scale, min_scale_of(circle), max_scale) : 1.0f;
    const float radius = r16 * scale;
    const float offset = std::isfinite(place->offset) ? place->offset : 0.0f;
    const float heights = place->anchor == 'r' ? aspect - offset : place->anchor == 'c' ? aspect * 0.5f + offset : offset;
    const float half_width = (std::min)(radius / aspect, 0.5f), half_height = (std::min)(radius, 0.5f);
    const float x = std::clamp(heights / aspect, half_width, 1.0f - half_width);
    const float y = std::clamp(std::isfinite(place->y) ? place->y : y16, half_height, 1.0f - half_height);
    return {x, y, radius};
}

// The floating stick's side of the image in a race, x from .. to: the left
// 40% while LS is on the left half, the right 40% once it is on the right
// half, reaching further in when a placed LS's circle (with its slack)
// does. LS at its default place keeps exactly the left 40% in any shape.
inline std::array<float, 2> race_stick_area(const std::optional<Place>& stick, float aspect) {
    if (!stick) return {0.0f, stick_side};
    if (!(aspect > 0.0f)) aspect = default_aspect;
    const auto [x, y, radius] = resolve(0, stick, aspect);
    const float reach = radius * touch_slack / aspect;
    if (x <= 0.5f) return {0.0f, (std::max)(stick_side, x + reach)};
    return {(std::min)(1.0f - stick_side, x - reach), 1.0f};
}

// To the decimals format writes, so what the editor shows is what is saved
// (the same decimal, read as parse_number reads it).
inline float quantize(float value, int decimals) {
    double unit = 1.0;
    for (int i = 0; i < decimals; ++i) unit *= 10.0;
    return float(std::round(double(value) * unit) / unit);
}

// The place of a control put at (x, y) in an image of that aspect with that
// size, kept whole on the image and anchored to the nearest of the left
// edge, the centre and the right edge.
inline Place place_at(size_t circle, float x, float y, float scale, float aspect) {
    if (!(aspect > 0.0f)) aspect = default_aspect;
    Place place;
    place.scale = std::clamp(scale, min_scale_of(circle), max_scale);
    const float radius = (circle < count ? defaults[circle][2] : 0.0f) * place.scale;
    const float half_width = (std::min)(radius / aspect, 0.5f), half_height = (std::min)(radius, 0.5f);
    const float heights = std::clamp(x, half_width, 1.0f - half_width) * aspect;
    place.y = std::clamp(y, half_height, 1.0f - half_height);
    const float left = heights, right = aspect - heights, centre = std::abs(heights - aspect * 0.5f);
    if (centre < left && centre < right) {
        place.anchor = 'c';
        place.offset = heights - aspect * 0.5f;
    } else if (left <= right) {
        place.offset = left;
    } else {
        place.anchor = 'r';
        place.offset = right;
    }
    place.offset = quantize(place.offset, 3);
    place.y = quantize(place.y, 3);
    place.scale = std::clamp(quantize(place.scale, 2), min_scale_of(circle), max_scale);
    return place;
}

// Plain decimals, without the C library: the locale's decimal comma cannot
// break them, and a settings file reads the same everywhere.
inline std::optional<float> parse_number(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    // The digits as one whole number, then a single division: the nearest
    // value to the decimal, as quantize makes it.
    long long digits = 0;
    double unit = 1.0;
    int count = 0;
    bool point = false;
    for (const char c : text) {
        if (c == '.' && !point) {
            point = true;
        } else if (c >= '0' && c <= '9') {
            if (++count > 12) return std::nullopt;
            digits = digits * 10 + (c - '0');
            if (point) unit *= 10.0;
        } else {
            return std::nullopt;
        }
    }
    const double value = double(digits) / unit;
    if (!count || value > 1000.0) return std::nullopt;
    return float(negative ? -value : value);
}

// value with that many decimals, "-0.250".
inline std::string format_number(float value, int decimals) {
    long long unit = 1;
    for (int i = 0; i < decimals; ++i) unit *= 10;
    const long long fixed = std::llround(double(value) * double(unit));
    const unsigned long long magnitude = fixed < 0 ? 0ull - (unsigned long long)fixed : (unsigned long long)fixed;
    std::string text = fixed < 0 ? "-" : "";
    text += std::to_string(magnitude / unit);
    if (decimals > 0) {
        std::string fraction = std::to_string(magnitude % unit);
        text += '.';
        text.append(size_t(decimals) - fraction.size(), '0');
        text += fraction;
    }
    return text;
}

// "ls:l0.320,0.550,1.40;a:r0.276,0.820,0.80" as a layout.
inline Layout parse(std::string_view list) {
    Layout layout;
    while (!list.empty()) {
        const size_t end = list.find(';');
        std::string_view entry = list.substr(0, end);
        list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
        const size_t colon = entry.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view name = entry.substr(0, colon), values = entry.substr(colon + 1);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
        const uint32_t bit = touch_options::bit_of(name);
        if (!bit) continue;
        size_t circle = 0;
        while ((1u << circle) != bit) ++circle;
        while (!values.empty() && (values.front() == ' ' || values.front() == '\t')) values.remove_prefix(1);
        if (values.empty()) continue;
        Place place;
        place.anchor = touch_options::lower(values.front());
        if (place.anchor != 'l' && place.anchor != 'r' && place.anchor != 'c') continue;
        values.remove_prefix(1);
        std::array<float, 3> numbers{};
        bool whole = true;
        for (size_t i = 0; i < numbers.size(); ++i) {
            const size_t comma = values.find(',');
            if ((i + 1 < numbers.size()) == (comma == std::string_view::npos)) { whole = false; break; }
            const auto number = parse_number(values.substr(0, comma));
            if (!number) { whole = false; break; }
            numbers[i] = *number;
            values = comma == std::string_view::npos ? std::string_view{} : values.substr(comma + 1);
        }
        if (!whole) continue;
        place.offset = numbers[0];
        place.y = numbers[1];
        place.scale = std::clamp(numbers[2], min_scale_of(circle), max_scale);
        layout.places[circle] = place;
    }
    return layout;
}

// The layout as parse reads it, in names' order; empty for the default.
inline std::string format(const Layout& layout) {
    std::string list;
    for (size_t i = 0; i < count; ++i) {
        const auto& place = layout.places[i];
        if (!place) continue;
        if (!list.empty()) list += ';';
        list += touch_options::names[i];
        list += ':';
        list += place->anchor;
        list += format_number(place->offset, 3) + ',' + format_number(place->y, 3) + ',' + format_number(place->scale, 2);
    }
    return list;
}
}

using TouchLayout = touch_layout::Layout;
}
