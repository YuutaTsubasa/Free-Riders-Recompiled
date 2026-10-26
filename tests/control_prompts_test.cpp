#include "control_prompts.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace p = sfr::prompts;
using sfr::InputAction;
using p::Action;
using p::Glyphs;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void equal(const std::string& got, const std::string& want, const char* what) {
    if (got != want) throw std::runtime_error(std::string(what) + ": got \"" + got + "\", want \"" + want + "\"");
}

// The race actions are the port's mapping (docs/race-controls.md).
void the_race_mapping() {
    const sfr::InputBindings pad = sfr::default_bindings(0);
    equal(p::prompt_text(Action::crouch, Glyphs::xbox, pad), "Hold A", "crouch");
    equal(p::prompt_text(Action::jump, Glyphs::xbox, pad), "Release A", "jump");
    equal(p::prompt_text(Action::fly, Glyphs::xbox, pad), "Hold A", "the fly skill (T-pose)");
    equal(p::prompt_text(Action::kick_dash, Glyphs::xbox, pad), "X", "kick dash");
    equal(p::prompt_text(Action::brake_grab, Glyphs::xbox, pad), "Hold B", "brake");
    equal(p::prompt_text(Action::stance, Glyphs::xbox, pad), "Y", "stance");
    equal(p::prompt_text(Action::power_skill, Glyphs::xbox, pad), "LB / RB", "power skill");
    equal(p::prompt_text(Action::item, Glyphs::xbox, pad), "RT", "item");
    equal(p::prompt_text(Action::shake_item, Glyphs::xbox, pad), "Hold RT", "shake");
    equal(p::prompt_text(Action::wipe, Glyphs::xbox, pad), "Hold RT + Right stick", "wipe");
    equal(p::prompt_text(Action::hold_hands, Glyphs::xbox, pad), "Hold LT", "hold hands");
    equal(p::prompt_text(Action::relay_swap, Glyphs::xbox, pad), "Back", "relay swap");
    equal(p::prompt_text(Action::steer, Glyphs::xbox, pad), "Left stick left / right", "steer");
    equal(p::prompt_text(Action::raise_hands, Glyphs::xbox, pad), "Right stick up", "raise hands");
    equal(p::prompt_text(Action::reach, Glyphs::xbox, pad), "Right stick left / right", "reach");
    equal(p::prompt_text(Action::air_trick, Glyphs::xbox, pad), "Spin Left stick", "air trick");
    equal(p::prompt_text(Action::menu_move, Glyphs::xbox, pad), "D-pad", "menu ring");
}

// The keyboard names the keys the player bound.
void keys_follow_the_bindings() {
    sfr::InputBindings keys = sfr::default_bindings(0);
    equal(p::prompt_text(Action::fly, Glyphs::keyboard, keys), "Hold Z", "fly on the keyboard");
    equal(p::prompt_text(Action::kick_dash, Glyphs::keyboard, keys), "C", "kick on the keyboard");
    equal(p::prompt_text(Action::steer, Glyphs::keyboard, keys), "Left / Right", "steer on the keyboard");
    equal(p::prompt_text(Action::air_trick, Glyphs::keyboard, keys), "Spin Up / Down / Left / Right", "trick keys");
    equal(p::prompt_text(Action::wipe, Glyphs::keyboard, keys), "Hold F + J / L", "wipe keys");
    keys[InputAction::a] = 0x20;  // Space
    equal(p::prompt_text(Action::crouch, Glyphs::keyboard, keys), "Hold Space", "a rebound key");
    keys[InputAction::a] = 0;
    equal(p::prompt_text(Action::crouch, Glyphs::keyboard, keys), "", "an unbound key has no prompt");
    require(p::prompt_pieces(Action::crouch, Glyphs::keyboard, keys).empty(), "nor pieces");
    const sfr::InputBindings second = sfr::default_bindings(1);
    equal(p::prompt_text(Action::crouch, Glyphs::keyboard, second), "Hold G", "the second player's keys");
}

// A rebound pad names the button that now stands for the title's control.
void pad_rebinding() {
    sfr::InputBindings pad = sfr::default_bindings(0);
    pad.from(InputAction::a) = InputAction::right_shoulder;  // RB is the title's A
    equal(p::prompt_text(Action::fly, Glyphs::xbox, pad), "Hold RB", "rebound A");
    equal(p::prompt_text(Action::fly, Glyphs::playstation, pad), "Hold R1", "rebound A on a PlayStation pad");
    const auto pieces = p::prompt_pieces(Action::fly, Glyphs::xbox, pad);
    require(pieces.size() == 2 && pieces[1].shape == p::Shape::bumper && pieces[1].label == "RB", "rebound A drawn as RB");
    pad.from(InputAction::x) = InputAction::count;
    equal(p::prompt_text(Action::kick_dash, Glyphs::xbox, pad), "", "nothing stands for X");
}

void playstation_and_touch_names() {
    const sfr::InputBindings pad = sfr::default_bindings(0);
    equal(p::prompt_text(Action::crouch, Glyphs::playstation, pad), "Hold Cross", "cross");
    equal(p::prompt_text(Action::brake_grab, Glyphs::playstation, pad), "Hold Circle", "circle");
    equal(p::prompt_text(Action::kick_dash, Glyphs::playstation, pad), "Square", "square");
    equal(p::prompt_text(Action::power_skill, Glyphs::playstation, pad), "L1 / R1", "L1 R1");
    equal(p::prompt_text(Action::hold_hands, Glyphs::playstation, pad), "Hold L2", "L2");
    equal(p::prompt_text(Action::pause, Glyphs::playstation, pad), "Options", "options");
    equal(p::prompt_text(Action::crouch, Glyphs::playstation, pad, 1), "按住 叉", "cross, Chinese");
    equal(p::prompt_text(Action::steer, Glyphs::touch, pad), "Stick left / right", "touch stick");
    equal(p::prompt_text(Action::reach, Glyphs::touch, pad), "RS left / right", "touch right stick");
    equal(p::prompt_text(Action::fly, Glyphs::touch, pad), "Hold A", "touch A");
    equal(p::prompt_text(Action::relay_swap, Glyphs::touch, pad), "", "no BACK on the touch controls");
    equal(p::prompt_text(Action::brake_grab, Glyphs::xbox, pad, 1), "按住 B", "Chinese hold");
    equal(p::prompt_text(Action::steer, Glyphs::xbox, pad, 1), "左搖桿 左 / 右", "Chinese stick");
}

// The drawn pieces: a word, then the button's picture.
void pieces() {
    const sfr::InputBindings pad = sfr::default_bindings(0);
    const auto fly = p::prompt_pieces(Action::fly, Glyphs::xbox, pad);
    require(fly.size() == 2, "HOLD and A");
    require(fly[0] == p::Piece{p::Shape::word, "HOLD", 0, 0}, "the word");
    require(fly[1].shape == p::Shape::face && fly[1].label == "A" && fly[1].colour == 0x3CB043, "a green A");
    const auto ps = p::prompt_pieces(Action::fly, Glyphs::playstation, pad);
    require(ps.size() == 2 && ps[1].shape == p::Shape::cross, "a cross");
    const auto steer = p::prompt_pieces(Action::steer, Glyphs::xbox, pad);
    require(steer.size() == 1 && steer[0].shape == p::Shape::stick && steer[0].label == "L" &&
            steer[0].arrows == (p::arrow::left | p::arrow::right), "one stick with both arrows");
    const auto keys = p::prompt_pieces(Action::steer, Glyphs::keyboard, pad);
    require(keys.size() == 3 && keys[0].label == "\x01" && keys[1].label == "/" && keys[2].label == "\x02",
            "the arrow keys as arrows");
    const auto trick = p::prompt_pieces(Action::air_trick, Glyphs::xbox, pad);
    require(trick.size() == 2 && trick[0].label == "SPIN" && trick[1].shape == p::Shape::stick && trick[1].arrows == 0,
            "SPIN and the stick");
    const auto space = [&] { sfr::InputBindings b = pad; b[InputAction::a] = 0x20; return b; }();
    require(p::prompt_pieces(Action::crouch, Glyphs::keyboard, space)[1].label == "SPACE", "a named key");
    const auto skill = p::prompt_pieces(Action::power_skill, Glyphs::touch, pad);
    require(skill.size() == 3 && skill[0].label == "LB" && skill[2].label == "RB", "touch LB / RB");
}

// The pixel art: sizes in units, rectangles in pixels, runs merged.
void drawing() {
    const sfr::InputBindings pad = sfr::default_bindings(0);
    const auto fly = p::prompt_pieces(Action::fly, Glyphs::xbox, pad);
    const p::Size size = p::prompt_size(fly);
    // HOLD (4 letters, 23 units), a gap of 3, the 11-unit button, 4 units of panel each side.
    require(size.width == 4 + 23 + 3 + 11 + 4 && size.height == 15, "the panel's size");
    const auto rects = p::prompt_rectangles(fly, 100, 50, 2);
    require(!rects.empty(), "something drawn");
    int left = 1 << 30, top = 1 << 30, right = 0, bottom = 0;
    bool green = false;
    for (const auto& r : rects) {
        require(r.left < r.right && r.top < r.bottom, "no empty rectangle");
        left = (std::min)(left, r.left); top = (std::min)(top, r.top);
        right = (std::max)(right, r.right); bottom = (std::max)(bottom, r.bottom);
        green = green || r.colour == 0x3CB043;
    }
    require(left == 100 && top == 50 && right == 100 + 2 * size.width && bottom == 50 + 2 * size.height,
            "the rectangles fill the panel at its place and scale");
    require(green, "the A is green");
    // The panel's middle row is one dark run where nothing is drawn on it.
    require(p::prompt_rectangles({}, 0, 0, 2).empty(), "no pieces, nothing drawn");
}

// Every action has a name in both languages and a prompt on every input
// (save the touch controls' missing BACK).
void every_action() {
    const sfr::InputBindings b = sfr::default_bindings(0);
    for (size_t i = 0; i < p::action_count; ++i) {
        const Action action = Action(i);
        require(*p::action_name(action, 0) && *p::action_name(action, 1), "a name for each action");
        for (const Glyphs glyphs : {Glyphs::xbox, Glyphs::playstation, Glyphs::keyboard, Glyphs::touch}) {
            if (glyphs == Glyphs::touch && action == Action::relay_swap) continue;
            require(!p::prompt_text(action, glyphs, b).empty(), std::string("a prompt for ") + p::action_name(action, 0));
            require(!p::prompt_pieces(action, glyphs, b).empty(), std::string("pieces for ") + p::action_name(action, 0));
        }
    }
}

// The pose figure's frames: which flip-book (by content) and which frame (by
// the centre of its texture coordinates) is which move. The coordinates are
// the ones traced in the tutorial (SFR_GUIDE_TRACE).
void figure_frames() {
    constexpr uint64_t start = 0x16C3D38A85972DAEull, lean = 0x4542D7607C162002ull, arm = 0xA27AB9299BB79DAFull,
                       reach = 0x0CF48ECA56F0F998ull, fly = 0xD040AD7B08BC7E86ull;
    for (const uint64_t book : {start, lean, arm, reach, fly}) require(p::pose_picture(book), "a pose flip-book");
    require(!p::pose_picture(0) && !p::pose_picture(0x1234), "anything else is not");
    require(!p::guide_action(0x1234, 0.5f, 0.5f), "no move for another texture");
    // Lesson 1, "Lean your body forward to go even faster!": standing
    // sideways first (nothing to press), then the lean forward.
    require(p::guide_action(start, 0.0898438f, 0.140625f) == Action::count, "standing sideways asks for nothing");
    for (const auto [u, v] : {std::pair{0.449219f, 0.421875f}, {0.628906f, 0.421875f}, {0.808594f, 0.421875f},
                              {0.0898438f, 0.703125f}, {0.269531f, 0.703125f}})
        require(p::guide_action(start, u, v) == Action::accelerate, "leaning forward speeds up");
    // Lesson 2: leaning forward, then back (which ends on the next flip-book).
    for (const auto [u, v] : {std::pair{0.640625f, 0.140625f}, {0.84375f, 0.140625f}, {0.234375f, 0.421875f},
                              {0.84375f, 0.421875f}, {0.234375f, 0.703125f}, {0.84375f, 0.703125f}})
        require(p::guide_action(lean, u, v) == Action::steer, "leaning is steering");
    require(p::guide_action(arm, 0.101562f, 0.140625f) == Action::steer &&
            p::guide_action(arm, 0.304688f, 0.140625f) == Action::steer, "the lean back's last frames");
    require(p::guide_action(fly, 0.77f, 0.14f) == Action::fly && p::guide_action(fly, 0.15f, 0.42f) == Action::fly,
            "arms spread: the fly skill");
    require(p::guide_action(reach, 0.5f, 0.42f) == Action::raise_hands, "both hands up");
    require(p::guide_action(reach, 0.1f, 0.70f) == Action::crouch && p::guide_action(reach, 0.3f, 0.70f) == Action::jump,
            "crouch, then jump");
    // The fly skill is A held, after the player's bindings.
    sfr::InputBindings pad = sfr::default_bindings(0);
    equal(p::prompt_text(*p::guide_action(fly, 0.77f, 0.14f), Glyphs::xbox, pad), "Hold A", "the T-pose's prompt");
}

// Where a prompt goes: centred under the figure, inside the window.
void placement() {
    const float view[4] = {0, 0, 1280, 720}, whole[2] = {1, 1};
    const p::Sighting s{0.25f, 0.25f, 0.45f, 0.70f, 0, Action::fly};
    const p::Size size{40, 15};
    const auto at = p::place_prompt(s, size, view, whole, 1280, 720);
    require(at.unit == 3, "3 pixels a unit at 720 lines");
    require(at.x == int(0.35f * 1280) - 60, "centred under the figure");
    require(at.y < int(0.70f * 720) && at.y > int(0.60f * 720), "at the figure's feet");
    // A figure at the very edge keeps its prompt in the window.
    const auto edge = p::place_prompt({0.0f, 0.8f, 0.02f, 1.0f, 0, Action::fly}, size, view, whole, 1280, 720);
    require(edge.x == 0 && edge.y + size.height * edge.unit <= 720, "kept inside");
    // A frame shown in part of the window (a letterboxed view) maps into it.
    const float letterbox[4] = {160, 0, 960, 540}, area[2] = {1, 1};
    const auto boxed = p::place_prompt(s, size, letterbox, area, 1280, 540);
    require(boxed.x == int(160 + 0.35f * 960) - 20 * boxed.unit, "inside the shown view");
}

// A frame drawn without the figure keeps the last prompt for a couple of
// presents, then drops it.
void sightings() {
    require(p::take_sightings().empty(), "nothing yet");
    p::note_sighting({0.1f, 0.1f, 0.2f, 0.2f, 0, Action::fly});
    require(p::take_sightings().size() == 1, "the figure of this frame");
    require(p::take_sightings().size() == 1 && p::take_sightings().size() == 1, "held over two frames without it");
    require(p::take_sightings().empty(), "then gone");
}

// Which glyphs a player's prompts use.
void devices() {
    require(p::device(1, false) == Glyphs::keyboard, "nothing yet: the keyboard");
    p::note_pad_present(1, true, Glyphs::playstation);
    require(p::device(1, false) == Glyphs::playstation, "a PlayStation pad plugged in");
    p::note_device(1, Glyphs::keyboard);
    require(p::device(1, false) == Glyphs::keyboard, "the keyboard used last");
    p::note_device(1, Glyphs::xbox);
    require(p::device(1, false) == Glyphs::playstation, "a pad used: its kind");
    require(p::device(0, true) == Glyphs::touch, "touch controls shown: their names");
}
}  // namespace

int main() {
    try {
        the_race_mapping();
        keys_follow_the_bindings();
        pad_rebinding();
        playstation_and_touch_names();
        pieces();
        drawing();
        every_action();
        figure_frames();
        placement();
        sightings();
        devices();
    } catch (const std::exception& error) {
        std::cerr << "control_prompts_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "control_prompts_test passed\n";
    return 0;
}
