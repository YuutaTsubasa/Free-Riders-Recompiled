#pragma once

namespace sfr::launcher {

enum class KeyboardStep {
    stay,
    open,       // ask for the keyboard (again, after a tap on the box)
    close,      // the text box was left
    leave_box,  // the player put the keyboard away: the box lets go too
};

// What the phones' keyboard does this frame. typing: a text box has the
// input; asked: the launcher opened the keyboard for it; text_input_active:
// SDL still takes text, which Android's Back turns off behind our back;
// tapped: a finger came up. Some keyboards close with a button of their own
// and tell nobody, so a tap on the box in use has to ask again.
constexpr KeyboardStep keyboard_step(bool typing, bool asked, bool text_input_active, bool tapped) {
    if (!typing) return asked ? KeyboardStep::close : KeyboardStep::stay;
    if (!asked) return KeyboardStep::open;
    if (!text_input_active) return KeyboardStep::leave_box;
    return tapped ? KeyboardStep::open : KeyboardStep::stay;
}

}  // namespace sfr::launcher
