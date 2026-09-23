#include "native_input.h"
#include "sony_gamepad.h"
#include "touch_controls.h"
#include "guest_memory.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#else
#include <SDL.h>
#include <mutex>
#include <vector>
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
    : pad_(std::move(pad)), keyboard_(std::move(keyboard)) {}

std::optional<GamepadState> NativeInput::current(uint32_t user) const {
    std::optional<GamepadState> state = pad_(user);
    if (user == 0) state = merge_gamepads(merge_gamepads(state.value_or(GamepadState{}), keyboard_()), script_());
    return state;
}

uint32_t NativeInput::get_state(GuestMemory& memory, uint32_t user, uint32_t output) {
    if (user > 3) throw RuntimeStop("native-input", user, "unsupported XamInputGetState user index");
    const std::optional<GamepadState> state = current(user);
    if (!state) return xinput_not_connected;
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

#ifdef _WIN32
NativeInput NativeInput::windows(std::function<void*()> focus_window, std::function<double()> script_clock) {
    auto pad = [](uint32_t user) -> std::optional<GamepadState> {
        XINPUT_STATE native{};
        if (XInputGetState(user, &native) != ERROR_SUCCESS)
            // No XInput pad: a PlayStation controller, if one is connected, is user 0.
            return user == 0 ? sony::latest() : std::nullopt;
        const auto& g = native.Gamepad;
        return GamepadState{g.wButtons, g.bLeftTrigger, g.bRightTrigger,
                            g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY};
    };
    auto keyboard = [focus_window = std::move(focus_window)]() {
        void* window = focus_window ? focus_window() : nullptr;
        if (!window || GetForegroundWindow() != static_cast<HWND>(window)) return GamepadState{};
        return keyboard_gamepad([](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; });
    };
    NativeInput input(pad, keyboard);
    input.attach_script(std::move(script_clock));
    input.vibrate_ = [](uint32_t user, uint16_t left, uint16_t right) {
        XINPUT_VIBRATION vibration{left, right};
        return XInputSetState(user, &vibration) == ERROR_SUCCESS;
    };
    return input;
}

NativeInput NativeInput::host(std::function<void*()> focus_window, std::function<double()> script_clock) {
    return windows(std::move(focus_window), std::move(script_clock));
}
#else
namespace {
// Open game controllers in connection order: the first is user 0.
struct SdlPads {
    std::mutex mutex;
    std::vector<SDL_GameController*> pads;
    int joysticks = -1;
    // Reopens the list when a controller comes or goes (caller holds mutex).
    void refresh() {
        const int count = SDL_NumJoysticks();
        const bool detached = std::any_of(pads.begin(), pads.end(),
                                          [](SDL_GameController* pad) { return !SDL_GameControllerGetAttached(pad); });
        if (count == joysticks && !detached) return;
        for (SDL_GameController* pad : pads) SDL_GameControllerClose(pad);
        pads.clear();
        joysticks = count;
        for (int i = 0; i < count && pads.size() < 4; ++i)
            if (SDL_IsGameController(i))
                if (SDL_GameController* pad = SDL_GameControllerOpen(i)) pads.push_back(pad);
    }
    SDL_GameController* get(uint32_t user) {
        refresh();
        return user < pads.size() ? pads[user] : nullptr;
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
    switch (key) {
    case 0x25: return SDL_SCANCODE_LEFT;
    case 0x26: return SDL_SCANCODE_UP;
    case 0x27: return SDL_SCANCODE_RIGHT;
    case 0x28: return SDL_SCANCODE_DOWN;
    case 0x0D: return SDL_SCANCODE_RETURN;
    case 0x09: return SDL_SCANCODE_TAB;
    case 0x20: return SDL_SCANCODE_SPACE;
    case 0x08: return SDL_SCANCODE_BACKSPACE;
    case 0x1B: return SDL_SCANCODE_ESCAPE;
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
    auto keyboard = [focus_window = std::move(focus_window)]() {
        // The on-screen touch controls (touch_controls.h) count as the keyboard.
        const GamepadState touch = touch_controls_active() ? touch_gamepad() : GamepadState{};
        void* window = focus_window ? focus_window() : nullptr;
        if (!window || SDL_GetKeyboardFocus() != static_cast<SDL_Window*>(window)) return touch;
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        return merge_gamepads(touch, keyboard_gamepad([keys](int key) {
            const SDL_Scancode code = scancode(key);
            // Android's Back button is Escape (B) as well.
            if (key == 0x1B && keys[SDL_SCANCODE_AC_BACK]) return true;
            return code != SDL_SCANCODE_UNKNOWN && keys[code] != 0;
        }));
    };
    NativeInput input(pad, keyboard);
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
    const char* script = std::getenv("SFR_INPUT_SCRIPT");
    if (!script || !*script) return;
    const auto started = std::chrono::steady_clock::now();
    scripted_gamepad(script, 0);  // validate before the title runs
    script_ = [text = std::string(script), started, script_clock = std::move(script_clock)] {
        const double seconds = script_clock ? script_clock()
            : std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return seconds < 0 ? GamepadState{} : scripted_gamepad(text, seconds);
    };
}
}
