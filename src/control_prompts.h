#pragma once
#include "input_bindings.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Button prompts (Issue #284): what each of the title's Kinect gestures is on
// the player's own input, for the launcher's list and for the small prompts
// drawn under the game's gesture pictures (native_presentation.cpp).
//
// The mapping is the port's (docs/race-controls.md, docs/pad-menus.md,
// nui_race.cpp): hold A to crouch and release it to jump, X kick dash, hold B
// brake or grab, and so on. A prompt names the control the player presses,
// after their own bindings (input_bindings.h): the pad button that stands
// for the title's A, the key bound to it, or the on-screen button.
namespace sfr::prompts {

// How controls are named: an Xbox pad's letters (the default for a pad),
// a PlayStation pad's shapes, key names, the on-screen buttons' names.
enum class Glyphs : uint8_t { xbox, playstation, keyboard, touch };

// What a player does, as the port maps the title's gestures.
enum class Action : uint8_t {
    crouch,       // hold A: crouch, charging a jump
    jump,         // release A
    kick_dash,    // X
    brake_grab,   // hold B: brake, grab a pole or a rail
    steer,        // left stick left / right: lean
    accelerate,   // left stick forward: lean forward to speed up (the board's acceleration)
    air_trick,    // spin the left stick in the air
    stance,       // Y: Regular / Goofy
    power_skill,  // LB / RB: the power skill, a punch to that side
    fly,          // hold A: the fly skill (T-pose), rise and keep flying
    item,         // RT: use an item
    shake_item,   // hold RT: shake an item
    wipe,         // hold RT and move the right stick: wipe the screen
    raise_hands,  // right stick up: both hands up
    reach,        // right stick left / right: reach for a ring beside the board
    hold_hands,   // hold LT, both players: hold hands (Tag Race)
    relay_swap,   // BACK: the next racer takes over (Relay Race)
    pause,        // START
    menu_choose,  // A
    menu_back,    // B
    menu_move,    // D-pad: turn the menu ring, move the focus
    menu_next,    // X: "next"
    menu_cursor,  // right stick: the Kinect hand cursor
    count
};
constexpr size_t action_count = size_t(Action::count);

// press: just the control; hold / release / spin: that word first; move: a
// stick or the D-pad pushed the way the arrows show.
enum class Verb : uint8_t { press, hold, release, move, spin };

// An action's controls: one, or two (second) either of which (join '/') or
// both together (join '+'), as the title's controls (InputAction).
struct Mapping {
    Verb verb;
    InputAction first;
    InputAction second = InputAction::count;
    char join = 0;
};
Mapping mapping(Action action);

// The action's name in the launcher's list, English (0) or Traditional
// Chinese (1).
const char* action_name(Action action, int language);

// The name of the control the player presses for the title's `control`:
// for a pad the button their bindings route to it ("A", or "Cross" on a
// PlayStation pad), the key bound to it, or the on-screen button. Empty when
// nothing is bound or the touch controls have no such button.
std::string control_name(InputAction control, Glyphs glyphs, const InputBindings& bindings, int language = 0);

// The prompt as text: "Hold A", "Hold Cross", "Hold Z", "Left stick left / right",
// "按住 A". Empty when a control is missing.
std::string prompt_text(Action action, Glyphs glyphs, const InputBindings& bindings, int language = 0);

// --- drawing ---------------------------------------------------------------

// One piece of a drawn prompt: a word, or a control's picture with a label.
enum class Shape : uint8_t {
    word,      // label in light letters
    face,      // a round face button, filled with colour, the label dark on it (Xbox, touch)
    cross, circle, square, triangle,  // a PlayStation face button: the shape on a dark disc
    bumper,    // a shoulder button or trigger: a dark rounded box, the label light
    stick,     // a stick: a dark disc with a light rim, label (L or R) on it, arrows beside
    dpad,      // the D-pad: a plus, arrows beside
    key,       // a key: a light box, the label dark
};
// Arrows beside a stick (none: any way, or round as SPIN says).
namespace arrow { constexpr uint8_t left = 1, right = 2, up = 4, down = 8; }
struct Piece {
    Shape shape = Shape::word;
    std::string label;     // upper case; '\x01'..'\x04' are the arrows left, right, up, down
    uint32_t colour = 0;   // 0xRRGGBB: a face button's fill, a PlayStation shape's colour
    uint8_t arrows = 0;
    bool operator==(const Piece&) const = default;
};
// The pieces of `action`'s prompt, or none when a control is missing.
std::vector<Piece> prompt_pieces(Action action, Glyphs glyphs, const InputBindings& bindings);

// A drawn prompt is pixel art: rectangles of solid colour, in units of one
// font pixel (`unit` screen pixels), the 5x7 letters and 11-unit-high
// buttons outlined in dark, with no panel under them. 0xRRGGBB colours.
struct Rect {
    int left, top, right, bottom;
    uint32_t colour;
    bool operator==(const Rect&) const = default;
};
// Its size in units (outline included).
struct Size { int width, height; };
Size prompt_size(const std::vector<Piece>& pieces);
// The rectangles with the panel's top-left corner at (x, y), each unit
// `unit` pixels; horizontal runs of one colour are merged.
std::vector<Rect> prompt_rectangles(const std::vector<Piece>& pieces, int x, int y, int unit);

// --- the game's pose figures ---------------------------------------------------

// A pose figure on screen: its box in the frame (fractions of the
// framebuffer's width and height, y down), whose figure it is, and what it
// asks for.
struct Sighting {
    float left, top, right, bottom;
    uint32_t player;
    Action action;
    bool operator==(const Sighting&) const = default;
};
// The figure's pose, by its picture. The title draws the figure from ten
// 1024x1024 flip-book textures, several moves each, frames packed where they
// fit; which texture it is is recognised by its content, wherever the title
// loaded it: 64 bytes of every KB of its megabyte (`sample(offset)` gives
// them, or null), FNV-1a. Then the frame, by the centre of the texture
// coordinates the draw samples (0..1), is a move: guide_action. None for a
// texture or a frame not in the table (a frame between two poses).
constexpr uint32_t picture_bytes = 0x100000, picture_step = 0x400, picture_sample = 64;
uint64_t picture_hash(const std::function<const uint8_t*(uint32_t offset)>& sample);
std::optional<Action> guide_action(uint64_t picture, float u, float v);
// Whether the texture is one of the pose flip-books at all.
bool pose_picture(uint64_t picture);
// Noted by the graphics hooks as the figure is drawn.
void note_sighting(const Sighting& sighting);
// At each present, the figures of the frame just drawn. A frame without any
// keeps the last ones for `hold` presents, so a frame drawn without its 2D
// (a frame the race skips drawing) does not make the prompt flicker.
std::vector<Sighting> take_sightings(int hold = 2);

// Where a prompt goes for a figure: centred under it, its top a little
// above the figure's box's bottom (the figure's feet; the box has a margin),
// kept inside the frame. In pixels of the window, for a frame shown in
// `view` (x, y, width, height), of which `area` (fractions) is the shown
// part. Returns the panel's top-left corner and the unit.
struct Placement { int x, y, unit; };
Placement place_prompt(const Sighting& sighting, Size size, const float view[4], const float area[2],
                       int window_width, int window_height);

// --- the players' input ------------------------------------------------------

// SFR_BUTTON_PROMPTS (launcher: Button prompts): 1 by default.
bool enabled();
// Each player's bindings, as the game was started with (native_input.cpp).
void set_bindings(uint32_t player, std::shared_ptr<const InputBindings> bindings);
std::shared_ptr<const InputBindings> bindings(uint32_t player);
// What the player last used: a pad (Xbox or PlayStation), the keyboard or
// the touch controls. Before anything is used, a connected pad's kind, else
// the touch controls where they are shown, else the keyboard.
void note_device(uint32_t player, Glyphs glyphs);
// The player's pad (its kind as note_pad_present last said) was used.
void note_pad_used(uint32_t player);
void note_pad_present(uint32_t player, bool present, Glyphs kind);
Glyphs device(uint32_t player, bool touch_shown);

}  // namespace sfr::prompts
