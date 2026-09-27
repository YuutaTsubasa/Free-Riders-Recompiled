#include "pad_devices.h"

#include "pad_assignment.h"
#include "sony_gamepad.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#else
#include <SDL.h>
#endif

namespace sfr {
namespace {

std::vector<PadDevice> look_for_pads() {
    std::vector<PadDevice> pads;
#ifdef _WIN32
    // XInput's four slots, named as a player would count them.
    for (uint32_t slot = 0; slot < 4; ++slot) {
        XINPUT_STATE state{};
        if (XInputGetState(slot, &state) != ERROR_SUCCESS) continue;
        pads.push_back({slot, "Controller " + std::to_string(slot + 1)});
    }
    // A PlayStation controller, which XInput cannot see at all.
    if (sony::latest()) pads.push_back({sony_pad_id, "PlayStation controller"});
#else
    std::vector<SdlPadIdentity> identities;
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        for (int index = 0; index < SDL_NumJoysticks(); ++index) {
            if (!SDL_IsGameController(index)) continue;
            const char* const name = SDL_GameControllerNameForIndex(index);
            char guid[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof guid);
            SDL_GameController* controller = SDL_GameControllerOpen(index);
            const char* serial = controller ? SDL_GameControllerGetSerial(controller) : nullptr;
            identities.push_back({uint64_t(SDL_JoystickGetDeviceInstanceID(index)),
                                  name && *name ? name : "Controller", guid, serial ? serial : ""});
            if (controller) SDL_GameControllerClose(controller);
        }
    }
    pads = identify_sdl_pads(identities);
#endif
    return pads;
}

}  // namespace

std::vector<PadDevice> identify_sdl_pads(std::span<const SdlPadIdentity> devices) {
    std::vector<PadDevice> result;
    for (size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        std::string identity = "sdl:" + device.guid;
        if (!device.serial.empty()) {
            // Stable, bounded encoding keeps setting values below 128 bytes.
            uint64_t hash = 14695981039346656037ull;
            for (const unsigned char c : device.serial) { hash ^= c; hash *= 1099511628211ull; }
            char serial[17];
            std::snprintf(serial, sizeof serial, "%016llx", static_cast<unsigned long long>(hash));
            identity += ":serial:" + std::string(serial);
        }
        size_t occurrence = 1, label_number = 1, same_names = 0;
        for (size_t other = 0; other < devices.size(); ++other) {
            if (devices[other].name == device.name) {
                ++same_names;
                if (other < index) ++label_number;
            }
            if (other < index && devices[other].guid == device.guid && devices[other].serial == device.serial) ++occurrence;
        }
        identity += ":" + std::to_string(occurrence);
        const std::string label = device.name + (same_names > 1 ? " (" + std::to_string(label_number) + ")" : "");
        result.push_back({device.id, label, identity, device.name});
    }
    return result;
}

std::vector<PadDevice> connected_pads() {
#ifndef _WIN32
    // SDL already keeps its device list in memory. A cached list here could
    // leave a just-connected preferred device absent until the next hotplug.
    return look_for_pads();
#else
    // Asking XInput about an empty slot is slow, and both the launcher's
    // list and the game's own look happen often, so the answer is kept for
    // a moment.
    static std::mutex lock;
    static std::vector<PadDevice> kept;
    static std::chrono::steady_clock::time_point looked{};
    static bool ever = false;
    std::lock_guard guard(lock);
    const auto now = std::chrono::steady_clock::now();
    if (!ever || now - looked >= std::chrono::milliseconds(200)) {
        kept = look_for_pads();
        looked = now;
        ever = true;
    }
    return kept;
#endif
}

uint64_t pad_with_name(const std::string& name) {
    return pad_with_name(name, connected_pads());
}

uint64_t pad_with_name(const std::string& name, std::span<const PadDevice> devices) {
    if (name.empty()) return PadAssignment::no_pad;
    for (const PadDevice& pad : devices)
        if (!pad.identity.empty() && pad.identity == name) return pad.id;
    uint64_t found = PadAssignment::absent_pad;
    for (const PadDevice& pad : devices) {
        const auto& legacy = pad.legacy_name.empty() ? pad.name : pad.legacy_name;
        if (legacy != name) continue;
        if (found != PadAssignment::absent_pad) return PadAssignment::absent_pad;
        found = pad.id;
    }
    return found;
}

std::vector<PadSample> poll_pad_buttons() {
    std::vector<PadSample> samples;
    for (const PadDevice& device : connected_pads()) {
#ifdef _WIN32
        if (device.id == sony_pad_id) {
            if (const auto state = sony::latest()) samples.push_back({device.id, *state});
            continue;
        }
        XINPUT_STATE native{};
        if (XInputGetState(uint32_t(device.id), &native) != ERROR_SUCCESS) continue;
        const auto& pad = native.Gamepad;
        samples.push_back({device.id, {pad.wButtons, pad.bLeftTrigger, pad.bRightTrigger}});
#else
        int index = 0;
        for (; index < SDL_NumJoysticks(); ++index)
            if (uint64_t(SDL_JoystickGetDeviceInstanceID(index)) == device.id) break;
        if (index == SDL_NumJoysticks()) continue;
        SDL_GameController* pad = SDL_GameControllerOpen(index);
        if (!pad) continue;
        SDL_GameControllerUpdate();
        static const std::pair<SDL_GameControllerButton, uint16_t> buttons[] = {
            {SDL_CONTROLLER_BUTTON_A, gamepad_button::a}, {SDL_CONTROLLER_BUTTON_B, gamepad_button::b},
            {SDL_CONTROLLER_BUTTON_X, gamepad_button::x}, {SDL_CONTROLLER_BUTTON_Y, gamepad_button::y},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, gamepad_button::left_shoulder},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, gamepad_button::right_shoulder},
            {SDL_CONTROLLER_BUTTON_START, gamepad_button::start}, {SDL_CONTROLLER_BUTTON_BACK, gamepad_button::back},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, gamepad_button::left_thumb}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, gamepad_button::right_thumb},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, gamepad_button::dpad_up}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, gamepad_button::dpad_down},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, gamepad_button::dpad_left}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, gamepad_button::dpad_right}};
        GamepadState state;
        for (const auto& [button, bit] : buttons)
            if (SDL_GameControllerGetButton(pad, button)) state.buttons |= bit;
        state.left_trigger = uint8_t((std::max)(int(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)), 0) >> 7);
        state.right_trigger = uint8_t((std::max)(int(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)), 0) >> 7);
        samples.push_back({device.id, state});
        SDL_GameControllerClose(pad);
#endif
    }
    return samples;
}

void PadButtonCapture::reset(std::span<const PadSample> samples) {
    previous_.assign(samples.begin(), samples.end());
}

std::optional<InputAction> PadButtonCapture::update(std::span<const PadSample> samples) {
    static constexpr uint16_t bits[] = {gamepad_button::a, gamepad_button::b, gamepad_button::x, gamepad_button::y,
        gamepad_button::left_shoulder, gamepad_button::right_shoulder, 0, 0, gamepad_button::start, gamepad_button::back,
        gamepad_button::left_thumb, gamepad_button::right_thumb, gamepad_button::dpad_up, gamepad_button::dpad_down,
        gamepad_button::dpad_left, gamepad_button::dpad_right};
    const auto down = [](const GamepadState& state, size_t action) {
        if (action == size_t(InputAction::left_trigger)) return state.left_trigger > 64;
        if (action == size_t(InputAction::right_trigger)) return state.right_trigger > 64;
        return (state.buttons & bits[action]) != 0;
    };
    std::optional<InputAction> pressed;
    for (const PadSample& sample : samples) {
        const auto previous = std::find_if(previous_.begin(), previous_.end(), [&](const PadSample& item) { return item.id == sample.id; });
        if (previous == previous_.end()) continue;
        for (size_t action = 0; action < size_t(InputAction::left_stick_up); ++action)
            if (!pressed && down(sample.state, action) && !down(previous->state, action)) pressed = InputAction(action);
    }
    reset(samples);
    return pressed;
}

}
