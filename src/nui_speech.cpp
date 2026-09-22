#include "nui_speech.h"
#include "guest_memory.h"
#include "native_input.h"
#include <string_view>

namespace sfr {
namespace {
constexpr uint32_t word_stride=32;  // bytes per word slot, room for 15 characters
}

NuiSpeechEmulation::Word NuiSpeechEmulation::hear(uint16_t pressed, bool racing) {
    if(pressed & gamepad_button::start) return racing ? Word::pause : Word::start;
    if(pressed & gamepad_button::a) return Word::ok;
    if(pressed & gamepad_button::b) return Word::back;
    // X says "next" in a race too: its step menus are drawn over the race
    // scene, and a race with no menu up has nothing that listens for the word
    // (X still kicks off through the body record).
    if(pressed & gamepad_button::x) return Word::next;
    if(pressed & gamepad_button::dpad_up) return Word::up;
    if(pressed & gamepad_button::dpad_down) return Word::down;
    return Word::none;
}

std::u16string_view NuiSpeechEmulation::text(Word word) {
    switch(word) {
    case Word::ok: return u"ok";
    case Word::back: return u"back";
    case Word::start: return u"start";
    // The title's word is "pauseopen" (its vocabulary has no "pause"): it
    // opens the pause ring in a race and in a replay (whose ring exits it).
    case Word::pause: return u"pauseopen";
    case Word::up: return u"up";
    case Word::down: return u"down";
    case Word::left: return u"left";
    case Word::right: return u"right";
    case Word::next: return u"next";
    case Word::none: break;
    }
    return {};
}

uint32_t NuiSpeechEmulation::address(Word word) {
    return word==Word::none ? 0 : words_address+uint32_t(word)*word_stride;
}

void NuiSpeechEmulation::write_words(GuestMemory& memory) {
    if(memory.available(words_address,words_size)) memory.map(words_address,words_size);
    for(auto word : {Word::ok,Word::back,Word::start,Word::pause,Word::up,Word::down,Word::left,Word::right,
                     Word::next}) {
        const auto characters=text(word);
        uint32_t at=address(word);
        for(char16_t c : characters) { memory.store<uint16_t>(at,uint16_t(c)); at+=2; }
        memory.store<uint16_t>(at,0);
    }
}

uint32_t NuiSpeechEmulation::say(GuestMemory& memory, std::string_view word) {
    // The slot after the mapped words, which write_words never touches.
    constexpr uint32_t scratch=words_address+words_size-64;
    if(memory.available(words_address,words_size)) memory.map(words_address,words_size);
    uint32_t at=scratch;
    for(char c : word.substr(0,31)) { memory.store<uint16_t>(at,uint16_t(uint8_t(c))); at+=2; }
    memory.store<uint16_t>(at,0);
    return scratch;
}

int dialog_choice(uint16_t pressed, uint32_t layout) {
    if(layout==4) {
        if(pressed & gamepad_button::a) return 0;
        if(pressed & gamepad_button::b) return 1;
    }
    if(layout==3 && (pressed & gamepad_button::a)) return 2;
    return -1;
}

int ring_step(uint16_t pressed) {
    const bool left=pressed & gamepad_button::dpad_left, right=pressed & gamepad_button::dpad_right;
    return left==right ? 0 : (left ? -1 : 1);
}

uint32_t menu_shortcut_command(uint32_t type) {
    switch(type) {
    case 33: return 15;
    case 34: return 16;
    case 35: return 49;
    default: return 0;
    }
}
}
