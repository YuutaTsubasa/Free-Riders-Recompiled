#pragma once
#include <cstdint>
#include <string_view>

namespace sfr {
class GuestMemory;

// Buttons newly pressed since the previous update.
class NuiPadEdges {
public:
    uint16_t update(uint16_t buttons) {
        const uint16_t pressed=buttons & ~previous_;
        previous_=buttons;
        return pressed;
    }
private:
    uint16_t previous_=0;
};

// Kinect voice commands from the pad. The title's input update (82494658)
// publishes the word the speech recognizer heard this frame at input+5440
// (a UTF-16 string pointer, cleared every frame) and its confidence level at
// +5448; the title screen (8244F880) and the menus' voice table (82434D30)
// compare it with their words (82A54AE0 is wcscmp). START is "start" (or
// "pauseopen" in a race or replay), A "ok", B "back", X "next" and D-pad up/down "up"/
// "down", from the generic menu vocabulary. D-pad left/right turn ring menus
// instead (ring_step), which also covers rings without voice words.
//
// "next" is the word a page with a Next button answers to: the gear parts
// page and the tutorial's step menu wait for it, and no other command moves
// them on.
class NuiSpeechEmulation {
public:
    enum class Word : uint8_t { none, ok, back, start, pause, up, down, left, right, next };
    // Guest page holding the words, outside the title's heap arenas.
    static constexpr uint32_t words_address=0x71720000, words_size=0x1000;

    static Word hear(uint16_t pressed, bool racing);
    static std::u16string_view text(Word);
    static uint32_t address(Word);  // 0 for none
    static void write_words(GuestMemory&);  // maps the page when missing
    // Writes an arbitrary word of the title's vocabulary into a scratch slot
    // and returns its address, for SFR_SAY (see nui_hooks.cpp).
    static uint32_t say(GuestMemory&, std::string_view word);
};

// Slot a pressed button chooses in a hand-pointer dialog of the given layout
// (823EF348: 4 = yes in slot 0 and no in slot 1, 3 = one button in slot 2),
// or -1: A confirms, B declines a two-button dialog.
int dialog_choice(uint16_t pressed, uint32_t layout);

// Items a pressed D-pad left (-1) or right (+1) turns a ring menu, or 0.
int ring_step(uint16_t pressed);

// Menu command (82454848) of a shortcut button type (33, 34, 35: the parts
// shop and its pages), or 0.
uint32_t menu_shortcut_command(uint32_t type);
}
