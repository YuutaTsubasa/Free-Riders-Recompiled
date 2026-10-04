#include "guest_memory.h"
#include "native_input.h"
#include "nui_race.h"
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
namespace button = sfr::gamepad_button;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

sfr::GamepadState pad(uint16_t buttons, int16_t lx = 0, int16_t ly = 0) {
    sfr::GamepadState state;
    state.buttons = buttons;
    state.thumb_lx = lx;
    state.thumb_ly = ly;
    return state;
}

void run(float steering_scale) {
    require(sfr::race_axis(7000, 7849) == 0 && sfr::race_axis(32767, 7849) == 1.0f &&
            sfr::race_axis(-32768, 7849) == -1.0f, "dead zone and full deflection");

    sfr::RaceInput race;
    race.update(pad(0), 1.0f / 60);
    require(race.body().a_held == 0 && race.body().lean_right == 1.0f && race.body().lean_left == 1.0f && race.body().hands == 0, "rest");
    race.update(pad(button::a), 1.0f / 60);
    require(race.body().a_held == 1 && race.body().a_released == 0 && race.body().hands == 2, "A held crouches");
    race.update(pad(0), 1.0f / 60);
    require(race.body().a_held == 0 && race.body().a_released == 1, "releasing A jumps once");
    race.update(pad(0), 1.0f / 60);
    require(race.body().a_released == 0, "the release lasts one frame");

    race.update(pad(button::x), 0.5f);
    require(race.body().x_pressed == 1 && near(race.body().x_held_seconds, 0.5f), "X press starts a kick");
    race.update(pad(button::x), 0.25f);
    require(race.body().x_pressed == 0 && near(race.body().x_held_seconds, 0.75f), "X hold time");
    race.update(pad(0), 0.25f);
    require(race.body().x_released == 1 && race.body().x_held_seconds == 0, "X release fires the kick");

    race.update(pad(0, 32767, 0), 1.0f / 60);
    require(race.body().left_x == 1 && race.body().lean == -1 && race.body().lean_right == 1.f + steering_scale && race.body().lean_left == 1.0f, "full right lean reaches the configured steering range");
    race.update(pad(0, -32768, -32768), 1.0f / 60);
    require(race.body().lean_left == 1.f + steering_scale && race.body().lean_right == 1.0f && race.body().left_y == -1 && race.body().hands == 2, "full left lean, pulled back");
    for (float seconds : {1.f / 60, 1.f / 30, 1.f / 20}) {
        race.update(pad(0, 20308), seconds); // halfway beyond the dead zone
        require(near(race.body().lean_right, 1.f + steering_scale * .5f) && race.body().lean_left == 1.f,
            "partial steering is proportional and independent of frame duration");
        require(near(race.body().left_x, .5f) && near(race.body().lean, -.5f),
            "tricks and other gestures keep normalized axes");
        race.update(pad(0), seconds);
        require(race.body().lean_right == 1 && race.body().lean_left == 1,
            "return to neutral does not accumulate steering");
    }

    race.update(pad(button::y | button::left_shoulder | button::b), 1.0f / 60);
    require(race.body().y_pressed == 1 && race.body().lb_pressed == 1 && race.body().rb_pressed == 0 &&
            race.body().b_held == 1, "Y, LB and B");
    race.update(pad(button::y | button::left_shoulder | button::b), 1.0f / 60);
    require(race.body().y_pressed == 0 && race.body().lb_pressed == 0 && race.body().b_held == 1, "presses last one frame");

    sfr::GamepadState trigger;
    trigger.right_trigger = 200;
    trigger.left_trigger = 20;
    race.update(trigger, 1.0f / 60);
    require(race.body().right_trigger == 1 && race.body().left_trigger == 0, "trigger threshold");

    sfr::GuestMemory memory;
    memory.map(0x10000000, 0x2000);
    race.update(pad(button::a | button::b, 16000, 32767), 1.0f / 60);
    race.write(memory, 0x10000000);
    auto f = [&](uint32_t offset) { return std::bit_cast<float>(memory.load<uint32_t>(0x10000000 + offset)); };
    require(f(8) == 1 && f(40) == 1 && near(f(88), race.body().left_x) && near(f(32), -race.body().left_x),
            "body record floats");
    require(memory.load<uint32_t>(0x10000000 + 268) == 1 && memory.load<uint32_t>(0x10000000 + 272) == 1 &&
            memory.load<uint32_t>(0x10000000 + 740) == 2 && memory.load<uint32_t>(0x10000000 + 756) == 2,
            "body record words");
    require(near(f(640), race.body().lean_right) && near(f(644), race.body().lean_left), "lean pair");
}
}

// The stick at an angle (degrees), fully deflected.
sfr::GamepadState stick_at(float degrees) {
    const float r = degrees * 3.14159265f / 180.0f;
    return pad(0, int16_t(std::cos(r) * 32767), int16_t(std::sin(r) * 32767));
}

void spins() {
    sfr::RaceInput race;
    const float frame = 1.0f / 60;
    // Steering from full left over the top to full right sweeps 180 degrees:
    // still steering, so the lean and the forward push stay.
    for (float a = 180; a >= 0; a -= 15) race.update(stick_at(a), frame);
    require(!race.body().spinning && race.body().left_x > .9f && race.body().lean_right > 1.0f,
            "steering across the top is not a spin");
    // Round and round (24 degrees a frame, about four turns a second).
    sfr::RaceInput spin;
    bool accelerated_while_spinning = false;
    for (int i = 0; i < 60; ++i) {
        spin.update(stick_at(-24.0f * i), frame);
        if (spin.body().spinning && spin.body().left_y >= 0.65f) accelerated_while_spinning = true;
    }
    require(spin.body().spinning, "circling the stick is a spin");
    require(!accelerated_while_spinning, "a spin never pushes the board forward");
    require(spin.body().left_x == 0 && spin.body().left_y == 0 && spin.body().lean_right == 1.0f &&
            spin.body().lean_left == 1.0f && spin.body().lean == 0, "a spin neither leans nor steers");
    require(spin.body().stick_x != 0 || spin.body().stick_y != 0, "the trick still sees the stick");
    // Held still, the spin ends after a fifth of a second and steering returns.
    for (int i = 0; i < 6; ++i) spin.update(stick_at(0), frame);
    require(spin.body().spinning, "a pause shorter than a fifth of a second keeps the spin");
    for (int i = 0; i < 12; ++i) spin.update(stick_at(0), frame);
    require(!spin.body().spinning && spin.body().left_x > .9f, "a still stick steers again");
    // Centred, the spin ends as well.
    for (int i = 0; i < 60; ++i) spin.update(stick_at(24.0f * i), frame);
    require(spin.body().spinning, "spinning the other way is a spin");
    for (int i = 0; i < 13; ++i) spin.update(pad(0), frame);
    require(!spin.body().spinning, "a centred stick ends the spin");
}

int main(int argc, char** argv) {
    try {
        run(argc > 1 ? std::strtof(argv[1], nullptr) : 3.5f);
        spins();
        std::cout << "nui race tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
