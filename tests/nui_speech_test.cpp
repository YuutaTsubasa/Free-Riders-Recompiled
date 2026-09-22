#include "guest_memory.h"
#include "native_input.h"
#include "nui_speech.h"
#include <iostream>
#include <stdexcept>

namespace {
namespace button = sfr::gamepad_button;
using Word = sfr::NuiSpeechEmulation::Word;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void run() {
    sfr::NuiPadEdges edges;
    require(edges.update(0) == 0, "nothing pressed at rest");
    require(edges.update(button::a) == button::a, "a new button is pressed");
    require(edges.update(button::a) == 0, "a held button is pressed once");
    require(edges.update(button::a | button::b) == button::b, "B pressed while A is held");
    require(edges.update(0) == 0, "release presses nothing");

    using Speech = sfr::NuiSpeechEmulation;
    require(Speech::hear(0, false) == Word::none, "silence without presses");
    require(Speech::hear(button::a, false) == Word::ok, "A says ok");
    require(Speech::hear(button::b, false) == Word::back, "B says back");
    require(Speech::hear(button::start, false) == Word::start, "START says start in menus");
    require(Speech::hear(button::start, true) == Word::pause && sfr::NuiSpeechEmulation::text(Word::pause) == u"pauseopen",
            "START says pauseopen in a race");
    require(Speech::hear(button::start | button::a, false) == Word::start, "START wins over A");
    require(Speech::hear(button::dpad_up, false) == Word::up && Speech::hear(button::dpad_down, false) == Word::down,
            "D-pad up and down say their direction");
    require(Speech::hear(button::dpad_left | button::dpad_right, false) == Word::none, "left and right turn rings instead");
    // A page with a Next button (the gear parts, the tutorial's steps) moves
    // on for this word alone.
    require(Speech::hear(button::x, false) == Word::next && Speech::hear(button::x, true) == Word::next,
            "X says next, in a race too: its step menus are drawn over the scene");
    require(sfr::ring_step(button::dpad_left) == -1 && sfr::ring_step(button::dpad_right) == 1 &&
            sfr::ring_step(button::dpad_left | button::dpad_right) == 0 && sfr::ring_step(button::a) == 0,
            "ring steps");
    require(sfr::menu_shortcut_command(33) == 15 && sfr::menu_shortcut_command(34) == 16 &&
            sfr::menu_shortcut_command(35) == 49 && sfr::menu_shortcut_command(31) == 0, "shortcut commands");
    require(Speech::hear(button::y | button::back, false) == Word::none, "other buttons are silent");

    require(sfr::dialog_choice(button::a, 4) == 0 && sfr::dialog_choice(button::b, 4) == 1, "A yes, B no");
    require(sfr::dialog_choice(button::a, 3) == 2 && sfr::dialog_choice(button::b, 3) == -1, "A takes the single button");
    require(sfr::dialog_choice(button::x, 4) == -1 && sfr::dialog_choice(button::a, 5) == -1, "no choice otherwise");

    sfr::GuestMemory memory;
    sfr::NuiSpeechEmulation::write_words(memory);
    sfr::NuiSpeechEmulation::write_words(memory);  // already mapped: rewrites in place
    for (auto word : {Word::ok, Word::back, Word::start, Word::pause, Word::up, Word::down, Word::left, Word::right,
                      Word::next}) {
        const auto text = sfr::NuiSpeechEmulation::text(word);
        const uint32_t at = sfr::NuiSpeechEmulation::address(word);
        for (size_t i = 0; i < text.size(); ++i)
            require(memory.load<uint16_t>(at + 2 * i) == text[i], "big-endian UTF-16 word");
        require(memory.load<uint16_t>(at + 2 * text.size()) == 0, "terminated word");
    }
    require(sfr::NuiSpeechEmulation::address(Word::none) == 0, "no word is a null pointer");
}
}

int main() {
    try {
        run();
        std::cout << "nui speech tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
