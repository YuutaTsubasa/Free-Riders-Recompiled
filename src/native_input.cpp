#include "native_input.h"
#include "input_bindings.h"
#include "pad_assignment.h"
#include "pad_devices.h"
#include "sony_gamepad.h"
#include "touch_controls.h"
#include "guest_memory.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#else
#include <SDL.h>
#endif

namespace sfr {
GamepadState keyboard_gamepad(const std::function<bool(int)>& down) {
    namespace button = gamepad_button;
    // Windows virtual-key codes; letters and digits use their ASCII values.
    constexpr int left = 0x25, up = 0x26, right = 0x27, bottom = 0x28;
    constexpr int enter = 0x0D, tab = 0x09, space = 0x20, backspace = 0x08, escape = 0x1B;
    GamepadState state;
    const auto map = [&](int key, uint16_t bits) { if (down(key)) state.buttons |= bits; };
    map(up, button::dpad_up);
    map(bottom, button::dpad_down);
    map(left, button::dpad_left);
    map(right, button::dpad_right);
    map(enter, button::start);
    map(tab, button::back);
    map('Z', button::a);
    map(space, button::a);
    map('X', button::b);
    map(backspace, button::b);
    map(escape, button::b);
    map('C', button::x);
    map('V', button::y);
    map('Q', button::left_shoulder);
    map('E', button::right_shoulder);
    // F/R: the triggers (RT uses and, held, shakes a race item).
    if (down('F')) state.right_trigger = 255;
    if (down('R')) state.left_trigger = 255;
    if (down(left) != down(right)) state.thumb_lx = down(left) ? -32768 : 32767;
    if (down(up) != down(bottom)) state.thumb_ly = down(up) ? 32767 : -32768;
    // I/J/K/L: the right stick (the Kinect menu cursor).
    if (down('J') != down('L')) state.thumb_rx = down('J') ? -32768 : 32767;
    if (down('I') != down('K')) state.thumb_ry = down('I') ? 32767 : -32768;
    return state;
}

GamepadState merge_gamepads(const GamepadState& first, const GamepadState& second) {
    const auto farther = [](int16_t a, int16_t b) { return std::abs(int{b}) > std::abs(int{a}) ? b : a; };
    GamepadState state;
    state.buttons = first.buttons | second.buttons;
    state.left_trigger = std::max(first.left_trigger, second.left_trigger);
    state.right_trigger = std::max(first.right_trigger, second.right_trigger);
    state.thumb_lx = farther(first.thumb_lx, second.thumb_lx);
    state.thumb_ly = farther(first.thumb_ly, second.thumb_ly);
    state.thumb_rx = farther(first.thumb_rx, second.thumb_rx);
    state.thumb_ry = farther(first.thumb_ry, second.thumb_ry);
    return state;
}

void write_xinput_state(GuestMemory& memory, uint32_t address, uint32_t packet, const GamepadState& state) {
    memory.check_write(address, xinput_state_size);
    memory.store<uint32_t>(address, packet);
    memory.store<uint16_t>(uint64_t(address) + 4, state.buttons);
    memory.store<uint8_t>(uint64_t(address) + 6, state.left_trigger);
    memory.store<uint8_t>(uint64_t(address) + 7, state.right_trigger);
    memory.store<uint16_t>(uint64_t(address) + 8, static_cast<uint16_t>(state.thumb_lx));
    memory.store<uint16_t>(uint64_t(address) + 10, static_cast<uint16_t>(state.thumb_ly));
    memory.store<uint16_t>(uint64_t(address) + 12, static_cast<uint16_t>(state.thumb_rx));
    memory.store<uint16_t>(uint64_t(address) + 14, static_cast<uint16_t>(state.thumb_ry));
}

GamepadState scripted_gamepad(const std::string& script, double seconds) {
    static const std::pair<const char*, uint16_t> names[] = {
        {"start", gamepad_button::start}, {"back", gamepad_button::back}, {"a", gamepad_button::a},
        {"b", gamepad_button::b}, {"x", gamepad_button::x}, {"y", gamepad_button::y},
        {"up", gamepad_button::dpad_up}, {"down", gamepad_button::dpad_down},
        {"left", gamepad_button::dpad_left}, {"right", gamepad_button::dpad_right},
        {"lb", gamepad_button::left_shoulder}, {"rb", gamepad_button::right_shoulder}};
    GamepadState state;
    size_t begin = 0;
    while (begin < script.size()) {
        const size_t end = std::min(script.find(',', begin), script.size());
        const std::string entry = script.substr(begin, end - begin);
        begin = end + 1;
        const size_t at = entry.find('@');
        if (at == std::string::npos) throw RuntimeStop("input-script", 0, "entry needs button@seconds: " + entry);
        const std::string name = entry.substr(0, at);
        const auto button = std::find_if(std::begin(names), std::end(names),
                                         [&](const auto& item) { return name == item.first; });
        // Stick directions: l/r then up, down, left or right (full deflection).
        static const std::pair<const char*, std::pair<int, int>> sticks[] = {
            {"lup", {0, 2}}, {"ldown", {0, 3}}, {"lleft", {0, 0}}, {"lright", {0, 1}},
            {"rup", {1, 2}}, {"rdown", {1, 3}}, {"rleft", {1, 0}}, {"rright", {1, 1}}};
        const auto stick = std::find_if(std::begin(sticks), std::end(sticks),
                                        [&](const auto& item) { return name == item.first; });
        if (button == std::end(names) && stick == std::end(sticks))
            throw RuntimeStop("input-script", 0, "unknown button: " + name);
        const size_t plus = entry.find('+', at);
        const double start = std::stod(entry.substr(at + 1, plus == std::string::npos ? std::string::npos : plus - at - 1));
        const double duration = plus == std::string::npos ? 0.25 : std::stod(entry.substr(plus + 1));
        if (seconds < start || seconds >= start + duration) continue;
        if (button != std::end(names)) { state.buttons |= button->second; continue; }
        const auto [side, direction] = stick->second;
        int16_t& x = side ? state.thumb_rx : state.thumb_lx;
        int16_t& y = side ? state.thumb_ry : state.thumb_ly;
        if (direction == 0) x = -32767;
        if (direction == 1) x = 32767;
        if (direction == 2) y = 32767;
        if (direction == 3) y = -32767;
    }
    return state;
}

NativeInput::NativeInput(std::function<std::optional<GamepadState>(uint32_t)> pad,
                         std::function<GamepadState()> keyboard)
    : pad_(std::move(pad)), keyboard_(std::move(keyboard)) {
    // The first player has the keyboard behind their pad, as they always
    // have; the others are pads only until somebody says otherwise.
    players_[0].device = uint8_t(PlayerDevice::both);
    for (size_t user = 1; user < players_.size(); ++user) players_[user].device = uint8_t(PlayerDevice::gamepad);
}

void NativeInput::set_player(uint32_t user, PlayerDevice device, std::shared_ptr<const InputBindings> pad,
                             std::function<GamepadState()> keyboard) {
    if (user >= players_.size()) return;
    players_[user] = {uint8_t(device), std::move(pad), std::move(keyboard)};
}

namespace {
bool uses_pad(uint8_t device) {
    const PlayerDevice which = PlayerDevice(device);
    return which == PlayerDevice::both || which == PlayerDevice::gamepad;
}
bool uses_keyboard(uint8_t device) {
    const PlayerDevice which = PlayerDevice(device);
    return which == PlayerDevice::both || which == PlayerDevice::keyboard;
}
}

std::optional<GamepadState> NativeInput::current(uint32_t user) const {
    if (user >= players_.size()) return std::nullopt;
    const Player& player = players_[user];
    std::optional<GamepadState> state;
    if (uses_pad(player.device)) {
        state = pad_(user);
        if (state && player.pad) state = remap_pad(*player.pad, *state);
    }
    if (uses_keyboard(player.device)) {
        // The first player's keyboard is the one the factory always made;
        // anybody else brings their own.
        const auto& keys = user == 0 ? keyboard_ : player.keyboard;
        if (keys) state = merge_gamepads(state.value_or(GamepadState{}), keys());
    }
    if (user == 0) state = merge_gamepads(state.value_or(GamepadState{}), script_());
    if (user == 1 && second_script_) state = merge_gamepads(state.value_or(GamepadState{}), second_script_());
    // The title polls user 0 every frame and stops when it is not there, so
    // that user stays connected even with every device taken away.
    if (user == 0 && !state) state = GamepadState{};
    return state;
}

std::optional<GamepadState> NativeInput::controller(uint32_t user) const {
    if (user >= players_.size()) return std::nullopt;
    const Player& player = players_[user];
    // A scripted second pad is a pad: the title counts its players by who is
    // holding one, so a script that only reached current() would move a
    // second player who never arrives. A player on the keyboard counts the
    // same way, which is what lets two people share one machine.
    std::optional<GamepadState> state;
    if (uses_pad(player.device)) {
        state = pad_(user);
        if (state && player.pad) state = remap_pad(*player.pad, *state);
    }
    if (user != 0 && uses_keyboard(player.device) && player.keyboard)
        state = merge_gamepads(state.value_or(GamepadState{}), player.keyboard());
    if (user == 1 && second_script_) state = merge_gamepads(state.value_or(GamepadState{}), second_script_());
    return state;
}

uint32_t NativeInput::get_state(GuestMemory& memory, uint32_t user, uint32_t output) {
    if (user > 3) throw RuntimeStop("native-input", user, "unsupported XamInputGetState user index");
    const std::optional<GamepadState> state = current(user);
    if (!state) return xinput_not_connected;
    std::lock_guard guard(*mutex_);
    // XInput semantics: the packet number changes only when the state changes.
    if (*state != last_[user]) {
        last_[user] = *state;
        ++packets_[user];
    }
    write_xinput_state(memory, output, packets_[user], *state);
    return xinput_success;
}

uint32_t NativeInput::set_vibration(uint32_t user, uint16_t left_motor, uint16_t right_motor) {
    if (user > 3) throw RuntimeStop("native-input", user, "unsupported XamInputSetState user index");
    const bool pad = vibrate_(user, left_motor, right_motor);
    return pad || user == 0 ? xinput_success : xinput_not_connected;
}

namespace {
// The Controls settings, as the launcher hands them over: which devices a
// player uses and how their keys and pad buttons are arranged
// (input_bindings.h). Nothing set means what this always did.
struct PlayerSetup {
    PlayerDevice device = PlayerDevice::both;
    std::shared_ptr<InputBindings> bindings;
    std::string gamepad;  // the controller asked for by name, or empty
};
PlayerSetup player_setup(uint32_t user) {
    const std::string prefix = user == 0 ? "SFR_PLAYER1_" : "SFR_PLAYER2_";
    PlayerSetup setup;
    setup.bindings = std::make_shared<InputBindings>(default_bindings(user));
    if (const char* text = std::getenv((prefix + "KEYS").c_str()); text && *text) read_keys(text, *setup.bindings);
    if (const char* text = std::getenv((prefix + "PAD").c_str()); text && *text) read_pad(text, *setup.bindings);
    const char* device = std::getenv((prefix + "INPUT").c_str());
    setup.device = player_device_from_name(device ? device : "",
                                           user == 0 ? PlayerDevice::both : PlayerDevice::gamepad);
    if (const char* text = std::getenv((prefix + "GAMEPAD").c_str()); text && *text) setup.gamepad = text;
    return setup;
}
}

#ifdef _WIN32
namespace {
// The host's controllers, each keeping the player number it was given
// (pad_assignment.h). A PlayStation pad is a controller of its own here
// rather than a stand-in for player one, which is what let a second pad take
// player one away from it.
struct WindowsPads {
    static constexpr uint64_t sony_pad = sony_pad_id;
    std::mutex mutex;
    PadAssignment assignment;
    // The controller each player asked for by name, if they did
    // (pad_devices.h); empty means whichever the host lists first.
    std::array<std::string, PadAssignment::players> wanted;
    std::chrono::steady_clock::time_point looked{};
    bool ever_looked = false;

    // Which controllers are here. XInput answers slowly for a slot with
    // nothing in it, so the four are only counted a few times a second; the
    // controller a player is already on is read every time it is asked for.
    void look() {
        const auto now = std::chrono::steady_clock::now();
        if (ever_looked && now - looked < std::chrono::milliseconds(250)) return;
        looked = now;
        ever_looked = true;
        const std::vector<PadDevice> devices = connected_pads();
        std::vector<uint64_t> connected;
        for (const PadDevice& device : devices) connected.push_back(device.id);
        for (uint32_t player = 0; player < PadAssignment::players; ++player)
            assignment.prefer(player, pad_with_name(wanted[player]));
        assignment.update(connected);
        announce(devices);
    }

    // Who ended up with what, printed whenever it changes -- the names here
    // are the ones the launcher offers and settings.ini keeps.
    std::array<uint64_t, PadAssignment::players> announced{~uint64_t(0), ~uint64_t(0), ~uint64_t(0), ~uint64_t(0)};
    bool ever_announced = false;
    void announce(const std::vector<PadDevice>& devices) {
        std::array<uint64_t, PadAssignment::players> now{};
        for (uint32_t player = 0; player < PadAssignment::players; ++player) now[player] = assignment.pad_of(player);
        if (ever_announced && now == announced) return;
        ever_announced = true;
        announced = now;
        for (uint32_t player = 0; player < PadAssignment::players; ++player) {
            if (now[player] == PadAssignment::no_pad) continue;
            const char* name = "?";
            for (const PadDevice& device : devices)
                if (device.id == now[player]) name = device.name.c_str();
            std::cerr << "NATIVE_PAD player=" << (player + 1) << " name=\"" << name << "\""
                      << (wanted[player].empty() ? " chosen=order" : " chosen=asked") << char(10);
        }
    }

    std::optional<GamepadState> read(uint32_t player) {
        std::lock_guard guard(mutex);
        look();
        const uint64_t pad = assignment.pad_of(player);
        if (pad == PadAssignment::no_pad) return std::nullopt;
        if (pad == sony_pad) return sony::latest();
        XINPUT_STATE native{};
        if (XInputGetState(uint32_t(pad), &native) != ERROR_SUCCESS) return std::nullopt;
        const auto& g = native.Gamepad;
        return GamepadState{g.wButtons, g.bLeftTrigger, g.bRightTrigger,
                            g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
    }

    // Only the XInput pads rumble; the PlayStation ones are read over HID and
    // nothing is written back to them.
    bool vibrate(uint32_t player, uint16_t left, uint16_t right) {
        std::lock_guard guard(mutex);
        look();
        const uint64_t pad = assignment.pad_of(player);
        if (pad == PadAssignment::no_pad || pad == sony_pad) return false;
        XINPUT_VIBRATION vibration{left, right};
        return XInputSetState(uint32_t(pad), &vibration) == ERROR_SUCCESS;
    }
};
}

NativeInput NativeInput::windows(std::function<void*()> focus_window, std::function<double()> script_clock) {
    const auto pads = std::make_shared<WindowsPads>();
    auto pad = [pads](uint32_t user) -> std::optional<GamepadState> { return pads->read(user); };
    // One reader per player, each with that player's own keys.
    const auto focus = std::make_shared<std::function<void*()>>(std::move(focus_window));
    const auto reader = [focus](std::shared_ptr<const InputBindings> bindings) {
        return [focus, bindings]() {
            void* window = *focus ? (*focus)() : nullptr;
            if (!window || GetForegroundWindow() != static_cast<HWND>(window)) return GamepadState{};
            return keyboard_state(*bindings, [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; });
        };
    };
    const PlayerSetup first = player_setup(0), second = player_setup(1);
    pads->wanted[0] = first.gamepad;
    pads->wanted[1] = second.gamepad;
    pads->assignment.enable(0, uses_pad(uint8_t(first.device)));
    pads->assignment.enable(1, uses_pad(uint8_t(second.device)));
    NativeInput input(pad, reader(first.bindings));
    input.set_player(0, first.device, first.bindings);
    input.set_player(1, second.device, second.bindings, reader(second.bindings));
    input.attach_script(std::move(script_clock));
    input.vibrate_ = [pads](uint32_t user, uint16_t left, uint16_t right) {
        return pads->vibrate(user, left, right);
    };
    return input;
}

NativeInput NativeInput::host(std::function<void*()> focus_window, std::function<double()> script_clock) {
    return windows(std::move(focus_window), std::move(script_clock));
}
#else
namespace {
// The host's controllers, each keeping the player number it was given
// (pad_assignment.h). SDL names a controller by an instance id that lasts as
// long as it is plugged in, so a controller that arrives or leaves does not
// move the others: the list is by joystick index, and those do move.
struct SdlPads {
    std::mutex mutex;
    std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> open;
    PadAssignment assignment;
    // The controller each player asked for by name (pad_devices.h).
    std::array<std::string, PadAssignment::players> wanted;
    bool wanted_changed = false;
    int joysticks = -1;

    // Opens what is new, closes what has gone (caller holds the mutex).
    void refresh() {
        const int count = SDL_NumJoysticks();
        const bool detached = std::any_of(open.begin(), open.end(),
                                          [](const auto& pad) { return !SDL_GameControllerGetAttached(pad.second); });
        if (count == joysticks && !detached && !wanted_changed) return;
        wanted_changed = false;
        joysticks = count;
        for (auto& pad : open)
            if (!SDL_GameControllerGetAttached(pad.second)) {
                SDL_GameControllerClose(pad.second);
                pad.second = nullptr;
            }
        std::erase_if(open, [](const auto& pad) { return !pad.second; });
        std::vector<uint64_t> connected;
        for (int index = 0; index < count; ++index) {
            if (!SDL_IsGameController(index)) continue;
            const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
            const auto found = std::find_if(open.begin(), open.end(), [&](const auto& pad) { return pad.first == id; });
            if (found == open.end()) {
                SDL_GameController* const pad = SDL_GameControllerOpen(index);
                if (!pad) continue;
                open.emplace_back(id, pad);
            }
            connected.push_back(uint64_t(id));
        }
        for (uint32_t player = 0; player < PadAssignment::players; ++player)
            assignment.prefer(player, pad_with_name(wanted[player]));
        assignment.update(connected);
    }

    SDL_GameController* get(uint32_t user) {
        refresh();
        const uint64_t id = assignment.pad_of(user);
        if (id == PadAssignment::no_pad) return nullptr;
        const auto found = std::find_if(open.begin(), open.end(),
                                        [&](const auto& pad) { return uint64_t(pad.first) == id; });
        return found == open.end() ? nullptr : found->second;
    }
};

GamepadState read_sdl_pad(SDL_GameController* pad) {
    static const std::pair<SDL_GameControllerButton, uint16_t> buttons[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, gamepad_button::dpad_up},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, gamepad_button::dpad_down},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, gamepad_button::dpad_left},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, gamepad_button::dpad_right},
        {SDL_CONTROLLER_BUTTON_START, gamepad_button::start},
        {SDL_CONTROLLER_BUTTON_BACK, gamepad_button::back},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, gamepad_button::left_thumb},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, gamepad_button::right_thumb},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, gamepad_button::left_shoulder},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, gamepad_button::right_shoulder},
        {SDL_CONTROLLER_BUTTON_A, gamepad_button::a},
        {SDL_CONTROLLER_BUTTON_B, gamepad_button::b},
        {SDL_CONTROLLER_BUTTON_X, gamepad_button::x},
        {SDL_CONTROLLER_BUTTON_Y, gamepad_button::y}};
    GamepadState state;
    for (const auto& [button, bit] : buttons)
        if (SDL_GameControllerGetButton(pad, button)) state.buttons |= bit;
    const auto axis = [&](SDL_GameControllerAxis which) { return SDL_GameControllerGetAxis(pad, which); };
    // SDL triggers run 0..32767; its stick Y axes point down, XInput's up.
    const auto trigger = [&](SDL_GameControllerAxis which) { return uint8_t(std::max<int>(axis(which), 0) >> 7); };
    const auto up = [&](SDL_GameControllerAxis which) { return int16_t(std::clamp(-int(axis(which)) - 1, -32768, 32767)); };
    state.left_trigger = trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    state.right_trigger = trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    state.thumb_lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
    state.thumb_ly = up(SDL_CONTROLLER_AXIS_LEFTY);
    state.thumb_rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
    state.thumb_ry = up(SDL_CONTROLLER_AXIS_RIGHTY);
    return state;
}

// keyboard_gamepad's Windows virtual keys as SDL scancodes.
SDL_Scancode scancode(int key) {
    if (key >= 'A' && key <= 'Z') return SDL_Scancode(SDL_SCANCODE_A + (key - 'A'));
    if (key >= '1' && key <= '9') return SDL_Scancode(SDL_SCANCODE_1 + (key - '1'));
    if (key >= 0x61 && key <= 0x69) return SDL_Scancode(SDL_SCANCODE_KP_1 + (key - 0x61));
    if (key >= 0x70 && key <= 0x7B) return SDL_Scancode(SDL_SCANCODE_F1 + (key - 0x70));
    switch (key) {
    case '0': return SDL_SCANCODE_0;
    case 0x10: return SDL_SCANCODE_LSHIFT;
    case 0x11: return SDL_SCANCODE_LCTRL;
    case 0x12: return SDL_SCANCODE_LALT;
    case 0x14: return SDL_SCANCODE_CAPSLOCK;
    case 0x21: return SDL_SCANCODE_PAGEUP;
    case 0x22: return SDL_SCANCODE_PAGEDOWN;
    case 0x23: return SDL_SCANCODE_END;
    case 0x24: return SDL_SCANCODE_HOME;
    case 0x25: return SDL_SCANCODE_LEFT;
    case 0x26: return SDL_SCANCODE_UP;
    case 0x27: return SDL_SCANCODE_RIGHT;
    case 0x28: return SDL_SCANCODE_DOWN;
    case 0x0D: return SDL_SCANCODE_RETURN;
    case 0x09: return SDL_SCANCODE_TAB;
    case 0x20: return SDL_SCANCODE_SPACE;
    case 0x08: return SDL_SCANCODE_BACKSPACE;
    case 0x1B: return SDL_SCANCODE_ESCAPE;
    case 0x2D: return SDL_SCANCODE_INSERT;
    case 0x2E: return SDL_SCANCODE_DELETE;
    case 0x60: return SDL_SCANCODE_KP_0;
    case 0x6A: return SDL_SCANCODE_KP_MULTIPLY;
    case 0x6B: return SDL_SCANCODE_KP_PLUS;
    case 0x6D: return SDL_SCANCODE_KP_MINUS;
    case 0x6E: return SDL_SCANCODE_KP_PERIOD;
    case 0x6F: return SDL_SCANCODE_KP_DIVIDE;
    case 0xBA: return SDL_SCANCODE_SEMICOLON;
    case 0xBB: return SDL_SCANCODE_EQUALS;
    case 0xBC: return SDL_SCANCODE_COMMA;
    case 0xBD: return SDL_SCANCODE_MINUS;
    case 0xBE: return SDL_SCANCODE_PERIOD;
    case 0xBF: return SDL_SCANCODE_SLASH;
    case 0xC0: return SDL_SCANCODE_GRAVE;
    case 0xDB: return SDL_SCANCODE_LEFTBRACKET;
    case 0xDC: return SDL_SCANCODE_BACKSLASH;
    case 0xDD: return SDL_SCANCODE_RIGHTBRACKET;
    case 0xDE: return SDL_SCANCODE_APOSTROPHE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}
}

NativeInput NativeInput::sdl(std::function<void*()> focus_window, std::function<double()> script_clock) {
    const bool controllers = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    auto pads = std::make_shared<SdlPads>();
    auto pad = [pads, controllers](uint32_t user) -> std::optional<GamepadState> {
        if (!controllers) return std::nullopt;
        std::lock_guard lock(pads->mutex);
        SDL_GameController* controller = pads->get(user);
        if (user == 0) note_controller(controller != nullptr);
        if (!controller) return std::nullopt;
        return read_sdl_pad(controller);
    };
    // One reader per player, each with that player's own keys. Only the
    // first player's carries the on-screen touch controls: they are one pair
    // of hands on one phone.
    const auto focus = std::make_shared<std::function<void*()>>(std::move(focus_window));
    const auto reader = [focus](std::shared_ptr<const InputBindings> bindings, bool touching) {
        return [focus, bindings, touching]() {
            const GamepadState touch = touching && touch_controls_active() ? touch_gamepad() : GamepadState{};
            void* window = *focus ? (*focus)() : nullptr;
            if (!window || SDL_GetKeyboardFocus() != static_cast<SDL_Window*>(window)) return touch;
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            return merge_gamepads(touch, keyboard_state(*bindings, [keys](int key) {
                const SDL_Scancode code = scancode(key);
                // Android's Back button is Escape (B) as well.
                if (key == 0x1B && keys[SDL_SCANCODE_AC_BACK]) return true;
                if (key == 0x10 && keys[SDL_SCANCODE_RSHIFT]) return true;
                if (key == 0x11 && keys[SDL_SCANCODE_RCTRL]) return true;
                if (key == 0x12 && keys[SDL_SCANCODE_RALT]) return true;
                if (key == 0x0D && keys[SDL_SCANCODE_KP_ENTER]) return true;
                return code != SDL_SCANCODE_UNKNOWN && keys[code] != 0;
            }));
        };
    };
    const PlayerSetup first = player_setup(0), second = player_setup(1);
    {
        std::lock_guard lock(pads->mutex);
        pads->wanted[0] = first.gamepad;
        pads->wanted[1] = second.gamepad;
        pads->assignment.enable(0, uses_pad(uint8_t(first.device)));
        pads->assignment.enable(1, uses_pad(uint8_t(second.device)));
        pads->wanted_changed = true;
    }
    NativeInput input(pad, reader(first.bindings, true));
    input.set_player(0, first.device, first.bindings);
    input.set_player(1, second.device, second.bindings, reader(second.bindings, false));
    input.attach_script(std::move(script_clock));
    input.vibrate_ = [pads, controllers](uint32_t user, uint16_t left, uint16_t right) {
        if (!controllers) return false;
        std::lock_guard lock(pads->mutex);
        SDL_GameController* controller = pads->get(user);
        // XInput motors keep their speed until changed: SDL's longest rumble.
        if (controller) SDL_GameControllerRumble(controller, left, right, 0xFFFF);
        return controller != nullptr;
    };
    return input;
}

NativeInput NativeInput::host(std::function<void*()> focus_window, std::function<double()> script_clock) {
    return sdl(std::move(focus_window), std::move(script_clock));
}
#endif

void NativeInput::attach_script(std::function<double()> script_clock) {
    const auto started = std::chrono::steady_clock::now();
    const auto reader = [started, script_clock](const char* text) {
        scripted_gamepad(text, 0);  // validate before the title runs
        return [script = std::string(text), started, script_clock]() {
            const double seconds = script_clock ? script_clock()
                : std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            return seconds < 0 ? GamepadState{} : scripted_gamepad(script, seconds);
        };
    };
    if (const char* script = std::getenv("SFR_INPUT_SCRIPT"); script && *script) script_ = reader(script);
    if (const char* script = std::getenv("SFR_INPUT_SCRIPT_2"); script && *script) second_script_ = reader(script);
}
}
