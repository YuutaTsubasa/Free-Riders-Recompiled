#include "launcher_screen_keyboard.h"

#include <cstdio>

using sfr::launcher::keyboard_step;
using sfr::launcher::KeyboardStep;

namespace {

int failures = 0;

void expect(KeyboardStep got, KeyboardStep wanted, const char* what) {
    if (got == wanted) return;
    std::fprintf(stderr, "%s: got %d, wanted %d\n", what, int(got), int(wanted));
    ++failures;
}

}  // namespace

int main() {
    expect(keyboard_step(false, false, false, false), KeyboardStep::stay, "no box, no keyboard");
    expect(keyboard_step(false, false, false, true), KeyboardStep::stay, "a tap elsewhere");
    expect(keyboard_step(true, false, false, true), KeyboardStep::open, "a box taken");
    expect(keyboard_step(true, false, false, false), KeyboardStep::open, "a box taken with the pad");
    expect(keyboard_step(true, true, true, false), KeyboardStep::stay, "typing");
    expect(keyboard_step(true, true, true, true), KeyboardStep::open, "the box tapped again");
    expect(keyboard_step(false, true, true, false), KeyboardStep::close, "the box left");
    expect(keyboard_step(true, true, false, false), KeyboardStep::leave_box, "Back");
    expect(keyboard_step(true, true, false, true), KeyboardStep::leave_box, "Back and a tap together");
    return failures ? 1 : 0;
}
