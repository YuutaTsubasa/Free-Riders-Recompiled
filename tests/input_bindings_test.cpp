#include "input_bindings.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
namespace button = sfr::gamepad_button;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

sfr::GamepadState pressed(const sfr::InputBindings& bindings, std::set<int> keys) {
    return sfr::keyboard_state(bindings, [&](int key) { return keys.count(key) != 0; });
}

// What the keyboard did before any of this existed, which is what the first
// player must still get without touching a setting.
void the_first_player_keeps_the_old_keys() {
    const sfr::InputBindings first = sfr::default_bindings(0);
    require(pressed(first, {0x0D}).buttons == button::start, "Enter is START");
    require(pressed(first, {'Z'}).buttons == button::a, "Z is A");
    require(pressed(first, {'X'}).buttons == button::b, "X is B");
    const auto up = pressed(first, {0x26});
    require(up.buttons == button::dpad_up && up.thumb_ly == 32767, "Up is the D-pad and the left stick");
    require(pressed(first, {'J'}).thumb_rx == -32768, "J is the right stick, left");
    require(pressed(first, {'F'}).right_trigger == 255 && pressed(first, {'R'}).left_trigger == 255,
            "F and R are the triggers");
    require(pressed(first, {0x25, 0x27}).thumb_lx == 0, "opposite directions cancel");
    require(pressed(first, {}) == sfr::GamepadState{}, "nothing held is rest");
}

// Two people on one keyboard: nothing the second player presses may be
// something the first player's hands are on.
void the_second_player_shares_the_keyboard() {
    const sfr::InputBindings first = sfr::default_bindings(0), second = sfr::default_bindings(1);
    std::set<int> theirs;
    for (size_t index = 0; index < sfr::input_action_count; ++index)
        if (first.key[index]) theirs.insert(first.key[index]);
    for (size_t index = 0; index < sfr::input_action_count; ++index)
        if (second.key[index]) require(!theirs.count(second.key[index]), "the two players' keys do not collide");
    require(pressed(second, {'W'}).thumb_ly == 32767 && pressed(second, {'G'}).buttons == button::a,
            "the second player steers with WASD and presses with G");
}

void bindings_survive_being_written_down() {
    sfr::InputBindings bindings = sfr::default_bindings(0);
    bindings[sfr::InputAction::a] = 'P';
    bindings.from(sfr::InputAction::a) = sfr::InputAction::b;
    const std::string keys = sfr::format_keys(bindings), pad = sfr::format_pad(bindings);
    require(keys.find("a=P") != std::string::npos, "a key is written by name");
    require(keys.find("start=Enter") != std::string::npos, "and so is a named key");
    require(pad.find("a=b") != std::string::npos, "the pad's source is written by name");
    sfr::InputBindings read;
    sfr::read_keys(keys, read);
    sfr::read_pad(pad, read);
    require(read.key == bindings.key && read.source == bindings.source, "and it all comes back");
    // A partial line changes only what it names.
    sfr::InputBindings some = sfr::default_bindings(0);
    sfr::read_keys("b=Space,nonsense=Q", some);
    require(some[sfr::InputAction::b] == 0x20 && some[sfr::InputAction::a] == 'Z',
            "an entry changes its own action and nothing else");
    sfr::read_keys("a=none", some);
    require(some[sfr::InputAction::a] == 0, "and a key can be taken away");
}

void every_key_survives_a_save() {
    for (int key = 1; key < 256; ++key) {
        auto bindings = sfr::default_bindings(0);
        bindings[sfr::InputAction::a] = key;
        sfr::InputBindings read;
        sfr::read_keys(sfr::format_keys(bindings), read);
        require(read.key == bindings.key, "every bindable key survives a save, including comma");
    }
}

void old_keyboard_aliases_remain_until_rebound() {
    auto first = sfr::default_bindings(0);
    require(pressed(first, {0x20}).buttons == button::a, "Space still confirms with default bindings");
    require(pressed(first, {0x08}).buttons == button::b && pressed(first, {0x1B}).buttons == button::b,
            "Backspace and Escape still go back with default bindings");
    first[sfr::InputAction::a] = 'P';
    first[sfr::InputAction::b] = 'O';
    require(pressed(first, {0x20, 0x08, 0x1B}).buttons == 0, "rebinding replaces the default aliases");
}

void a_pad_button_can_stand_for_another() {
    sfr::InputBindings bindings;
    require(sfr::pad_is_unchanged(bindings), "a pad answers for itself until it is rebound");
    sfr::GamepadState raw;
    raw.buttons = button::x;
    raw.thumb_lx = 1234;
    raw.right_trigger = 200;
    require(sfr::remap_pad(bindings, raw) == raw, "and is passed through untouched");

    // Swap A and X, and put START on the left shoulder.
    bindings.from(sfr::InputAction::a) = sfr::InputAction::x;
    bindings.from(sfr::InputAction::x) = sfr::InputAction::a;
    bindings.from(sfr::InputAction::start) = sfr::InputAction::left_shoulder;
    const sfr::GamepadState swapped = sfr::remap_pad(bindings, raw);
    require(swapped.buttons == button::a, "the X button now presses A");
    require(swapped.thumb_lx == 1234 && swapped.right_trigger == 200, "the sticks and triggers are left alone");
    // A control given away stops answering for itself: the left shoulder is
    // START now, not START and LB.
    sfr::GamepadState shoulder;
    shoulder.buttons = button::left_shoulder;
    require(sfr::remap_pad(bindings, shoulder).buttons == button::start, "and the shoulder is START alone");
    // The button that lost its action does nothing rather than keeping it.
    sfr::GamepadState none;
    none.buttons = button::y;
    require(sfr::remap_pad(bindings, none).buttons == button::y, "a button nobody rebound still answers for itself");
}

void a_trigger_given_away_stops_its_original_action() {
    for (const auto trigger : {sfr::InputAction::left_trigger, sfr::InputAction::right_trigger}) {
        sfr::InputBindings bindings;
        bindings.from(sfr::InputAction::start) = trigger;
        sfr::GamepadState raw;
        if (trigger == sfr::InputAction::left_trigger) raw.left_trigger = 200;
        else raw.right_trigger = 200;
        const auto mapped = sfr::remap_pad(bindings, raw);
        require(mapped.buttons == button::start, "a rebound trigger presses START");
        require(mapped.left_trigger == 0 && mapped.right_trigger == 0,
                "a trigger given to START no longer also presses its original action");
        raw.left_trigger = raw.right_trigger = 30;
        const auto below_threshold = sfr::remap_pad(bindings, raw);
        require(below_threshold.buttons == 0, "a rebound trigger keeps the press threshold");
        require((trigger == sfr::InputAction::left_trigger ? below_threshold.left_trigger : below_threshold.right_trigger) == 0,
                "a rebound trigger does not leak an analog value below the threshold");
        require((trigger == sfr::InputAction::left_trigger ? below_threshold.right_trigger : below_threshold.left_trigger) == 30,
                "the other trigger preserves its analog value");
    }
}

void devices_and_names() {
    require(std::string(sfr::input_action_name(sfr::InputAction::right_stick_up)) == "right_stick_up", "action names");
    require(sfr::input_action_from_name("left_shoulder") == sfr::InputAction::left_shoulder, "and back");
    require(!sfr::input_action_from_name("elbow"), "an unknown name is refused");
    require(sfr::key_name(0x68) == "Numpad 8" && sfr::key_from_name("numpad 8") == 0, "key names are exact");
    require(sfr::key_from_name("Numpad 8") == 0x68 && sfr::key_from_name("z") == 'Z', "a letter may be either case");
    require(sfr::key_from_name("200") == 200 && sfr::key_from_name("999") == 0, "a number is a key, in range");
    require(sfr::player_device_from_name("keyboard", sfr::PlayerDevice::both) == sfr::PlayerDevice::keyboard,
            "a device is read by name");
    require(sfr::player_device_from_name("", sfr::PlayerDevice::gamepad) == sfr::PlayerDevice::gamepad,
            "and an empty one keeps what it was");
    require(std::string(sfr::player_device_name(sfr::PlayerDevice::off)) == "off", "and written back");
}
}

int main() {
    try {
        the_first_player_keeps_the_old_keys();
        the_second_player_shares_the_keyboard();
        bindings_survive_being_written_down();
        a_pad_button_can_stand_for_another();
        a_trigger_given_away_stops_its_original_action();
        devices_and_names();
        every_key_survives_a_save();
        old_keyboard_aliases_remain_until_rebound();
        std::cout << "input binding checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
