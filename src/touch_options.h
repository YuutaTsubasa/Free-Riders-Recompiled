#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sfr {
// Which on-screen touch controls are drawn and which carry their names
// (Issue #443): a player who steers by tilting has no use for the stick, and
// one who knows the buttons may want only some names. The launcher keeps two
// lists of control names in settings.ini (touch_hidden, touch_unnamed) and
// hands them to the game as SFR_TOUCH_HIDDEN and SFR_TOUCH_UNNAMED. Names,
// not positions, so a later layout editor can key a control's place and size
// by the same names. A hidden control is neither drawn nor read: its part of
// the screen is empty screen. "Button names" off (SFR_TOUCH_LABELS=0) still
// hides every name at once.
//
// Header only: the launcher uses it without the game's touch reader.
namespace touch_options {
// In TouchControls::Button order (touch_controls.h checks it): the circle
// index is the bit.
inline constexpr std::array<std::string_view, 11> names{"ls", "a", "b", "x", "y", "rt", "start", "lb", "rb", "lt", "rs"};
inline constexpr uint32_t all = (1u << names.size()) - 1;

inline constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

// The bit of a control's name (any case), 0 for no control.
inline constexpr uint32_t bit_of(std::string_view name) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i].size() != name.size()) continue;
        bool same = true;
        for (size_t c = 0; c < name.size(); ++c) same = same && lower(name[c]) == names[i][c];
        if (same) return 1u << i;
    }
    return 0;
}

// "ls,lt" as a mask; spaces around names and unknown names are ignored.
inline constexpr uint32_t parse(std::string_view list) {
    uint32_t mask = 0;
    while (!list.empty()) {
        const size_t comma = list.find(',');
        std::string_view name = list.substr(0, comma);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.remove_suffix(1);
        mask |= bit_of(name);
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return mask;
}

// A mask as the list parse reads, in names' order; empty for none.
inline std::string format(uint32_t mask) {
    std::string list;
    for (size_t i = 0; i < names.size(); ++i)
        if (mask & (1u << i)) {
            if (!list.empty()) list += ',';
            list += names[i];
        }
    return list;
}
}

// What the game was told: hidden controls, controls without their names.
struct TouchOptions {
    uint32_t hidden = 0;
    uint32_t unnamed = 0;
    bool shown(size_t circle) const { return (hidden & (1u << circle)) == 0; }
    bool named(size_t circle) const { return (unnamed & (1u << circle)) == 0; }
};
}
