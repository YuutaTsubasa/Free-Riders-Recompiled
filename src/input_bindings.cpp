#include "input_bindings.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace sfr {
namespace {

constexpr std::array<const char*, input_action_count> action_names{
    "a", "b", "x", "y",
    "left_shoulder", "right_shoulder", "left_trigger", "right_trigger",
    "start", "back", "left_thumb", "right_thumb",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right"};

// The keys worth a name. Letters and digits are themselves; the rest are
// Windows virtual-key codes.
struct NamedKey { int key; const char* name; };
constexpr NamedKey named_keys[] = {
    {0x08, "Backspace"}, {0x09, "Tab"}, {0x0D, "Enter"}, {0x10, "Shift"}, {0x11, "Ctrl"}, {0x12, "Alt"},
    {0x14, "Caps Lock"}, {0x1B, "Escape"}, {0x20, "Space"}, {0x21, "Page Up"}, {0x22, "Page Down"},
    {0x23, "End"}, {0x24, "Home"}, {0x25, "Left"}, {0x26, "Up"}, {0x27, "Right"}, {0x28, "Down"},
    {0x2D, "Insert"}, {0x2E, "Delete"},
    {0x60, "Numpad 0"}, {0x61, "Numpad 1"}, {0x62, "Numpad 2"}, {0x63, "Numpad 3"}, {0x64, "Numpad 4"},
    {0x65, "Numpad 5"}, {0x66, "Numpad 6"}, {0x67, "Numpad 7"}, {0x68, "Numpad 8"}, {0x69, "Numpad 9"},
    {0x6A, "Numpad *"}, {0x6B, "Numpad +"}, {0x6D, "Numpad -"}, {0x6E, "Numpad ."}, {0x6F, "Numpad /"},
    {0x70, "F1"}, {0x71, "F2"}, {0x72, "F3"}, {0x73, "F4"}, {0x74, "F5"}, {0x75, "F6"},
    {0x76, "F7"}, {0x77, "F8"}, {0x78, "F9"}, {0x79, "F10"}, {0x7A, "F11"}, {0x7B, "F12"},
    {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."}, {0xBF, "/"}, {0xC0, "`"},
    {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"}, {0xDE, "'"}};

std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

// Walks "name=value,name=value", handing each pair over.
void for_each_pair(std::string_view text, const std::function<void(std::string_view, std::string_view)>& body) {
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto entry = text.substr(0, comma);
        if (const auto equals = entry.find('='); equals != std::string_view::npos)
            body(entry.substr(0, equals), entry.substr(equals + 1));
        if (comma == std::string_view::npos) break;
        text = text.substr(comma + 1);
    }
}

const int16_t stick_low = -32768, stick_high = 32767;

}  // namespace

const char* input_action_name(InputAction action) {
    return size_t(action) < input_action_count ? action_names[size_t(action)] : "";
}

std::optional<InputAction> input_action_from_name(std::string_view name) {
    const std::string wanted = trimmed(name);
    for (size_t index = 0; index < input_action_count; ++index)
        if (wanted == action_names[index]) return InputAction(index);
    return std::nullopt;
}

std::string key_name(int key) {
    if (!key) return "none";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, char(key));
    for (const NamedKey& named : named_keys)
        if (named.key == key) return named.name;
    char text[16];
    std::snprintf(text, sizeof text, "%d", key);
    return text;
}

int key_from_name(std::string_view name) {
    const std::string wanted = trimmed(name);
    if (wanted.empty() || wanted == "none") return 0;
    if (wanted.size() == 1) {
        const char c = wanted[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    for (const NamedKey& named : named_keys)
        if (wanted == named.name) return named.key;
    int value = 0;
    const auto [end, error] = std::from_chars(wanted.data(), wanted.data() + wanted.size(), value);
    if (error == std::errc() && end == wanted.data() + wanted.size() && value > 0 && value < 256) return value;
    return 0;
}

InputBindings::InputBindings() {
    key.fill(0);
    // A pad that has not been rebound answers for itself.
    for (size_t index = 0; index < input_action_count; ++index) source[index] = InputAction(index);
}

InputBindings default_bindings(uint32_t player) {
    InputBindings bindings;
    if (player == 0) {
        // What the keyboard has always been (native_input.h): arrows for the
        // D-pad and the left stick, IJKL for the right stick, Z/X for A/B.
        bindings[InputAction::a] = 'Z';
        bindings[InputAction::b] = 'X';
        bindings[InputAction::x] = 'C';
        bindings[InputAction::y] = 'V';
        bindings[InputAction::left_shoulder] = 'Q';
        bindings[InputAction::right_shoulder] = 'E';
        bindings[InputAction::left_trigger] = 'R';
        bindings[InputAction::right_trigger] = 'F';
        bindings[InputAction::start] = 0x0D;  // Enter
        bindings[InputAction::back] = 0x09;   // Tab
        bindings[InputAction::dpad_up] = 0x26;
        bindings[InputAction::dpad_down] = 0x28;
        bindings[InputAction::dpad_left] = 0x25;
        bindings[InputAction::dpad_right] = 0x27;
        bindings[InputAction::left_stick_up] = 0x26;
        bindings[InputAction::left_stick_down] = 0x28;
        bindings[InputAction::left_stick_left] = 0x25;
        bindings[InputAction::left_stick_right] = 0x27;
        bindings[InputAction::right_stick_up] = 'I';
        bindings[InputAction::right_stick_down] = 'K';
        bindings[InputAction::right_stick_left] = 'J';
        bindings[InputAction::right_stick_right] = 'L';
        return bindings;
    }
    // The second player shares the keyboard with the first, so none of these
    // may be one of the first player's: the left hand steers with WASD, the
    // buttons sit under it, and the right stick is the number pad.
    bindings[InputAction::a] = 'G';
    bindings[InputAction::b] = 'H';
    bindings[InputAction::x] = 'T';
    bindings[InputAction::y] = 'Y';
    bindings[InputAction::left_shoulder] = '1';
    bindings[InputAction::right_shoulder] = '2';
    bindings[InputAction::left_trigger] = '3';
    bindings[InputAction::right_trigger] = '4';
    bindings[InputAction::start] = '5';
    bindings[InputAction::back] = '6';
    bindings[InputAction::dpad_up] = 'W';
    bindings[InputAction::dpad_down] = 'S';
    bindings[InputAction::dpad_left] = 'A';
    bindings[InputAction::dpad_right] = 'D';
    bindings[InputAction::left_stick_up] = 'W';
    bindings[InputAction::left_stick_down] = 'S';
    bindings[InputAction::left_stick_left] = 'A';
    bindings[InputAction::left_stick_right] = 'D';
    bindings[InputAction::right_stick_up] = 0x68;     // Numpad 8
    bindings[InputAction::right_stick_down] = 0x62;   // Numpad 2
    bindings[InputAction::right_stick_left] = 0x64;   // Numpad 4
    bindings[InputAction::right_stick_right] = 0x66;  // Numpad 6
    return bindings;
}

std::string format_keys(const InputBindings& bindings) {
    std::string out;
    for (size_t index = 0; index < input_action_count; ++index) {
        if (!out.empty()) out += ',';
        out += action_names[index];
        out += '=';
        // A comma is the entry separator. Small unnamed codes need a
        // leading zero to distinguish them from the digit keys.
        const int key = bindings.key[index];
        if (key == 0xBC) out += "188";
        else if (key > 0 && key < 8) out += "0" + std::to_string(key);
        else out += key_name(key);
    }
    return out;
}

std::string format_pad(const InputBindings& bindings) {
    std::string out;
    for (size_t index = 0; index < input_action_count; ++index) {
        if (!out.empty()) out += ',';
        out += action_names[index];
        out += '=';
        out += size_t(bindings.source[index]) < input_action_count ? input_action_name(bindings.source[index]) : "none";
    }
    return out;
}

void read_keys(std::string_view text, InputBindings& bindings) {
    for_each_pair(text, [&](std::string_view name, std::string_view value) {
        if (const auto action = input_action_from_name(name)) bindings[*action] = key_from_name(value);
    });
}

void read_pad(std::string_view text, InputBindings& bindings) {
    for_each_pair(text, [&](std::string_view name, std::string_view value) {
        const auto action = input_action_from_name(name);
        if (!action) return;
        const std::string wanted = trimmed(value);
        if (wanted == "none") { bindings.from(*action) = InputAction::count; return; }
        if (const auto source = input_action_from_name(wanted)) bindings.from(*action) = *source;
    });
}

namespace {
// Puts one action into a pad state. Buttons are bits; the triggers are
// whole; a stick direction pushes its axis all the way.
void apply(GamepadState& state, InputAction action) {
    namespace button = gamepad_button;
    switch (action) {
    case InputAction::a: state.buttons |= button::a; break;
    case InputAction::b: state.buttons |= button::b; break;
    case InputAction::x: state.buttons |= button::x; break;
    case InputAction::y: state.buttons |= button::y; break;
    case InputAction::left_shoulder: state.buttons |= button::left_shoulder; break;
    case InputAction::right_shoulder: state.buttons |= button::right_shoulder; break;
    case InputAction::left_trigger: state.left_trigger = 255; break;
    case InputAction::right_trigger: state.right_trigger = 255; break;
    case InputAction::start: state.buttons |= button::start; break;
    case InputAction::back: state.buttons |= button::back; break;
    case InputAction::left_thumb: state.buttons |= button::left_thumb; break;
    case InputAction::right_thumb: state.buttons |= button::right_thumb; break;
    case InputAction::dpad_up: state.buttons |= button::dpad_up; break;
    case InputAction::dpad_down: state.buttons |= button::dpad_down; break;
    case InputAction::dpad_left: state.buttons |= button::dpad_left; break;
    case InputAction::dpad_right: state.buttons |= button::dpad_right; break;
    case InputAction::left_stick_up: state.thumb_ly = stick_high; break;
    case InputAction::left_stick_down: state.thumb_ly = stick_low; break;
    case InputAction::left_stick_left: state.thumb_lx = stick_low; break;
    case InputAction::left_stick_right: state.thumb_lx = stick_high; break;
    case InputAction::right_stick_up: state.thumb_ry = stick_high; break;
    case InputAction::right_stick_down: state.thumb_ry = stick_low; break;
    case InputAction::right_stick_left: state.thumb_rx = stick_low; break;
    case InputAction::right_stick_right: state.thumb_rx = stick_high; break;
    default: break;
    }
}

// Whether a pad is holding an action, for remapping one to another.
bool holding(const GamepadState& state, InputAction action) {
    namespace button = gamepad_button;
    constexpr int16_t reach = 16384;  // half way is far enough to mean "pushed"
    switch (action) {
    case InputAction::a: return (state.buttons & button::a) != 0;
    case InputAction::b: return (state.buttons & button::b) != 0;
    case InputAction::x: return (state.buttons & button::x) != 0;
    case InputAction::y: return (state.buttons & button::y) != 0;
    case InputAction::left_shoulder: return (state.buttons & button::left_shoulder) != 0;
    case InputAction::right_shoulder: return (state.buttons & button::right_shoulder) != 0;
    case InputAction::left_trigger: return state.left_trigger > 64;
    case InputAction::right_trigger: return state.right_trigger > 64;
    case InputAction::start: return (state.buttons & button::start) != 0;
    case InputAction::back: return (state.buttons & button::back) != 0;
    case InputAction::left_thumb: return (state.buttons & button::left_thumb) != 0;
    case InputAction::right_thumb: return (state.buttons & button::right_thumb) != 0;
    case InputAction::dpad_up: return (state.buttons & button::dpad_up) != 0;
    case InputAction::dpad_down: return (state.buttons & button::dpad_down) != 0;
    case InputAction::dpad_left: return (state.buttons & button::dpad_left) != 0;
    case InputAction::dpad_right: return (state.buttons & button::dpad_right) != 0;
    case InputAction::left_stick_up: return state.thumb_ly > reach;
    case InputAction::left_stick_down: return state.thumb_ly < -reach;
    case InputAction::left_stick_left: return state.thumb_lx < -reach;
    case InputAction::left_stick_right: return state.thumb_lx > reach;
    case InputAction::right_stick_up: return state.thumb_ry > reach;
    case InputAction::right_stick_down: return state.thumb_ry < -reach;
    case InputAction::right_stick_left: return state.thumb_rx < -reach;
    case InputAction::right_stick_right: return state.thumb_rx > reach;
    default: return false;
    }
}
}  // namespace

GamepadState keyboard_state(const InputBindings& bindings, const std::function<bool(int)>& down) {
    GamepadState state;
    // Keep the release's alternate confirm/back keys while those actions
    // still have their original bindings. Explicit rebinding replaces them.
    if (bindings[InputAction::a] == 'Z' && down(0x20)) state.buttons |= gamepad_button::a;
    if (bindings[InputAction::b] == 'X' && (down(0x08) || down(0x1B))) state.buttons |= gamepad_button::b;
    for (size_t index = 0; index < input_action_count; ++index) {
        const int key = bindings.key[index];
        if (key && down(key)) apply(state, InputAction(index));
    }
    // Opposite directions cancel rather than fighting over the axis.
    const auto cancel = [&](InputAction low, InputAction high, int16_t& axis) {
        const int first = bindings.key[size_t(low)], second = bindings.key[size_t(high)];
        if (first && second && down(first) && down(second)) axis = 0;
    };
    cancel(InputAction::left_stick_left, InputAction::left_stick_right, state.thumb_lx);
    cancel(InputAction::left_stick_down, InputAction::left_stick_up, state.thumb_ly);
    cancel(InputAction::right_stick_left, InputAction::right_stick_right, state.thumb_rx);
    cancel(InputAction::right_stick_down, InputAction::right_stick_up, state.thumb_ry);
    return state;
}

bool pad_is_unchanged(const InputBindings& bindings) {
    for (size_t index = 0; index < input_action_count; ++index)
        if (bindings.source[index] != InputAction(index)) return false;
    return true;
}

GamepadState remap_pad(const InputBindings& bindings, const GamepadState& raw) {
    if (pad_is_unchanged(bindings)) return raw;
    GamepadState state;
    // A control assigned to another action no longer answers for itself.
    std::array<bool, input_action_count> taken{};
    for (size_t index = 0; index < input_action_count; ++index) {
        const InputAction source = bindings.source[index];
        if (size_t(source) < input_action_count && source != InputAction(index)) taken[size_t(source)] = true;
    }
    // The sticks stay sticks: they are read as positions, not as presses, so
    // an unbound stick keeps its own axis rather than losing it.
    const auto kept = [&](InputAction action) { return bindings.source[size_t(action)] == action; };
    if (kept(InputAction::left_stick_left) && kept(InputAction::left_stick_right)) state.thumb_lx = raw.thumb_lx;
    if (kept(InputAction::left_stick_up) && kept(InputAction::left_stick_down)) state.thumb_ly = raw.thumb_ly;
    if (kept(InputAction::right_stick_left) && kept(InputAction::right_stick_right)) state.thumb_rx = raw.thumb_rx;
    if (kept(InputAction::right_stick_up) && kept(InputAction::right_stick_down)) state.thumb_ry = raw.thumb_ry;
    if (kept(InputAction::left_trigger) && !taken[size_t(InputAction::left_trigger)]) state.left_trigger = raw.left_trigger;
    if (kept(InputAction::right_trigger) && !taken[size_t(InputAction::right_trigger)]) state.right_trigger = raw.right_trigger;
    // An action still on its own control is already carried above when it is
    // one of the analogue ones; every other action is a press, and a press
    // is a press whether or not it was moved.
    const auto analogue = [](InputAction action) {
        return action >= InputAction::left_stick_up || action == InputAction::left_trigger
               || action == InputAction::right_trigger;
    };
    // A control that has been given to another action stops answering for
    // its own: binding START to the left shoulder moves it there rather than
    // making that button do both.
    for (size_t index = 0; index < input_action_count; ++index) {
        const InputAction action = InputAction(index), source = bindings.source[index];
        if (size_t(source) >= input_action_count) continue;
        if (source == action && (analogue(action) || taken[index])) continue;
        if (holding(raw, source)) apply(state, action);
    }
    return state;
}

const char* player_device_name(PlayerDevice device) {
    switch (device) {
    case PlayerDevice::both: return "both";
    case PlayerDevice::gamepad: return "gamepad";
    case PlayerDevice::keyboard: return "keyboard";
    default: return "off";
    }
}

PlayerDevice player_device_from_name(std::string_view name, PlayerDevice fallback) {
    const std::string wanted = trimmed(name);
    if (wanted == "both") return PlayerDevice::both;
    if (wanted == "gamepad") return PlayerDevice::gamepad;
    if (wanted == "keyboard") return PlayerDevice::keyboard;
    if (wanted == "off") return PlayerDevice::off;
    return fallback;
}

}
