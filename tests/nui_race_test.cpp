#include "guest_memory.h"
#include "native_input.h"
#include "nui_race.h"
#include <bit>
#include <cmath>
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

void run() {
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
    require(race.body().left_x == 1 && race.body().lean == -1 && race.body().lean_right == 2.0f && race.body().lean_left == 1.0f, "full right lean");
    race.update(pad(0, -32768, -32768), 1.0f / 60);
    require(race.body().lean_left == 2.0f && race.body().lean_right == 1.0f && race.body().left_y == -1 && race.body().hands == 2, "left lean, pulled back");

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

int main() {
    try {
        run();
        std::cout << "nui race tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
