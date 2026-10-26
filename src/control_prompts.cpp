#include "control_prompts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>

namespace sfr::prompts {
namespace {

using IA = InputAction;

bool left_stick(IA control) { return control >= IA::left_stick_up && control <= IA::left_stick_right; }
bool right_stick(IA control) { return control >= IA::right_stick_up && control <= IA::right_stick_right; }
bool stick(IA control) { return left_stick(control) || right_stick(control); }
bool dpad(IA control) { return control >= IA::dpad_up && control <= IA::dpad_right; }

// The four directions of the stick or D-pad `control` is one of, in the
// order up, down, left, right (InputAction's).
std::array<IA, 4> directions_of(IA control) {
    const IA first = left_stick(control) ? IA::left_stick_up : right_stick(control) ? IA::right_stick_up : IA::dpad_up;
    return {first, IA(size_t(first) + 1), IA(size_t(first) + 2), IA(size_t(first) + 3)};
}

uint8_t arrow_of(IA control) {
    switch ((size_t(control) - size_t(IA::dpad_up)) % 4) {
    case 0: return arrow::up;
    case 1: return arrow::down;
    case 2: return arrow::left;
    default: return arrow::right;
    }
}

struct Entry { Mapping mapping; const char* names[2]; };
// In Action's order.
const std::array<Entry, action_count> entries{{
    {{Verb::hold, IA::a}, {"Crouch (charge a jump)", "蹲下（蓄力）"}},
    {{Verb::release, IA::a}, {"Jump", "跳躍"}},
    {{Verb::press, IA::x}, {"Kick dash", "踢地加速（Kick Dash）"}},
    {{Verb::hold, IA::b}, {"Brake, grab a pole or a rail", "煞車、抓住桿子或軌道"}},
    {{Verb::move, IA::left_stick_left, IA::left_stick_right, '/'}, {"Steer (lean)", "傾斜轉彎"}},
    {{Verb::move, IA::left_stick_up}, {"Lean forward to speed up", "身體前傾加速"}},
    {{Verb::spin, IA::left_stick_up}, {"Air trick: start spinning before you release the jump", "空中特技：放開跳躍前就開始轉"}},
    {{Verb::press, IA::y}, {"Stance (Regular / Goofy)", "切換站姿（Regular／Goofy）"}},
    {{Verb::press, IA::left_shoulder, IA::right_shoulder, '/'}, {"Power skill (punch left / right)", "左右招式（Power Skill）"}},
    {{Verb::hold, IA::a}, {"Fly skill (T-pose): rise, keep flying", "飛行招式（T 字姿勢）：上升、維持飛行"}},
    {{Verb::press, IA::right_trigger}, {"Use an item", "使用道具"}},
    {{Verb::hold, IA::right_trigger}, {"Shake an item", "搖晃道具"}},
    {{Verb::hold, IA::right_trigger, IA::right_stick_left, '+'}, {"Wipe the screen", "擦螢幕"}},
    {{Verb::move, IA::right_stick_up}, {"Raise your hands", "舉起雙手"}},
    {{Verb::move, IA::right_stick_left, IA::right_stick_right, '/'}, {"Reach left / right for rings", "往左／右伸手拿 Ring"}},
    {{Verb::hold, IA::left_trigger}, {"Hold hands (Tag Race, both players)", "牽手（Tag Race，兩位玩家都按）"}},
    {{Verb::press, IA::back}, {"Swap racers (Relay Race)", "換下一位選手（Relay Race）"}},
    {{Verb::press, IA::start}, {"Pause", "暫停"}},
    {{Verb::press, IA::a}, {"Menus: choose", "選單：選取"}},
    {{Verb::press, IA::b}, {"Menus: back", "選單：返回"}},
    {{Verb::move, IA::dpad_up}, {"Menus: turn the ring, move the focus", "選單：轉動圓環、移動焦點"}},
    {{Verb::press, IA::x}, {"Menus: Next (tutorial pages and similar)", "選單：Next（下一步）"}},
    {{Verb::move, IA::right_stick_up}, {"Menus: the hand cursor", "選單：手部游標"}},
}};

// Actions whose stick or D-pad is used in every direction.
bool all_directions(Action action) {
    return action == Action::air_trick || action == Action::menu_move || action == Action::menu_cursor;
}

// --- names -------------------------------------------------------------------

const char* xbox_name(IA control, int language) {
    static const std::array<std::array<const char*, 2>, input_action_count> names{{
        {"A", "A"}, {"B", "B"}, {"X", "X"}, {"Y", "Y"}, {"LB", "LB"}, {"RB", "RB"}, {"LT", "LT"}, {"RT", "RT"},
        {"Start", "Start"}, {"Back", "Back"}, {"Left stick press", "左搖桿按下"}, {"Right stick press", "右搖桿按下"},
        {"D-pad up", "十字鍵 上"}, {"D-pad down", "十字鍵 下"}, {"D-pad left", "十字鍵 左"}, {"D-pad right", "十字鍵 右"},
        {"Left stick up", "左搖桿 上"}, {"Left stick down", "左搖桿 下"}, {"Left stick left", "左搖桿 左"},
        {"Left stick right", "左搖桿 右"}, {"Right stick up", "右搖桿 上"}, {"Right stick down", "右搖桿 下"},
        {"Right stick left", "右搖桿 左"}, {"Right stick right", "右搖桿 右"}}};
    return names[size_t(control)][language ? 1 : 0];
}
const char* playstation_name(IA control, int language) {
    switch (control) {
    case IA::a: return language ? "叉" : "Cross";
    case IA::b: return language ? "圈" : "Circle";
    case IA::x: return language ? "方塊" : "Square";
    case IA::y: return language ? "三角" : "Triangle";
    case IA::left_shoulder: return "L1";
    case IA::right_shoulder: return "R1";
    case IA::left_trigger: return "L2";
    case IA::right_trigger: return "R2";
    case IA::start: return "Options";
    case IA::back: return "Share";
    case IA::left_thumb: return "L3";
    case IA::right_thumb: return "R3";
    default: return xbox_name(control, language);
    }
}

// The pad control that stands for the title's `control`, or count.
IA pad_source(IA control, const InputBindings& bindings) { return bindings.from(control); }

const char* stick_name(IA control, int language) {
    if (left_stick(control)) return language ? "左搖桿" : "Left stick";
    if (right_stick(control)) return language ? "右搖桿" : "Right stick";
    return language ? "十字鍵" : "D-pad";
}
const char* direction_name(IA control, int language) {
    static const char* names[2][4] = {{"up", "down", "left", "right"}, {"上", "下", "左", "右"}};
    return names[language ? 1 : 0][(size_t(control) - size_t(IA::dpad_up)) % 4];
}

const char* touch_name(IA control) {
    switch (control) {
    case IA::a: return "A";
    case IA::b: return "B";
    case IA::x: return "X";
    case IA::y: return "Y";
    case IA::left_shoulder: return "LB";
    case IA::right_shoulder: return "RB";
    case IA::left_trigger: return "LT";
    case IA::right_trigger: return "RT";
    case IA::start: return "START";
    default: return "";
    }
}

}  // namespace

Mapping mapping(Action action) { return entries[size_t(action)].mapping; }

const char* action_name(Action action, int language) {
    return size_t(action) < action_count ? entries[size_t(action)].names[language ? 1 : 0] : "";
}

std::string control_name(InputAction control, Glyphs glyphs, const InputBindings& bindings, int language) {
    if (size_t(control) >= input_action_count) return {};
    switch (glyphs) {
    case Glyphs::keyboard: {
        const int key = bindings[control];
        return key ? key_name(key) : std::string();
    }
    case Glyphs::touch:
        if (left_stick(control) || dpad(control)) return language ? "搖桿" : "Stick";
        if (right_stick(control)) return "RS";
        return touch_name(control);
    case Glyphs::xbox:
    case Glyphs::playstation: {
        // The sticks are not rebound (input_bindings.h: a pad's sticks stay sticks).
        const IA source = stick(control) ? control : pad_source(control, bindings);
        if (size_t(source) >= input_action_count) return {};
        return glyphs == Glyphs::xbox ? xbox_name(source, language) : playstation_name(source, language);
    }
    }
    return {};
}

std::string prompt_text(Action action, Glyphs glyphs, const InputBindings& bindings, int language) {
    if (size_t(action) >= action_count) return {};
    const Mapping m = mapping(action);
    std::string out;
    static const char* verbs[2][5] = {{"", "Hold ", "Release ", "", "Spin "}, {"", "按住 ", "放開 ", "", "轉動 "}};
    out += verbs[language ? 1 : 0][size_t(m.verb)];
    const bool keys = glyphs == Glyphs::keyboard;
    // One control's words: a stick (or the touch stick) by its name and the
    // way it is pushed, anything else by its own name.
    const auto one = [&](IA control) -> std::string {
        if (!keys && (stick(control) || dpad(control)) && glyphs != Glyphs::touch) {
            if (all_directions(action)) return stick_name(control, language);
            return std::string(stick_name(control, language)) + " " + direction_name(control, language);
        }
        if (glyphs == Glyphs::touch && (stick(control) || dpad(control)) && !all_directions(action)) {
            const std::string name = control_name(control, glyphs, bindings, language);
            return name.empty() ? name : name + " " + direction_name(control, language);
        }
        return control_name(control, glyphs, bindings, language);
    };
    if (keys && all_directions(action)) {
        // The four keys of the stick or D-pad.
        std::string list;
        for (const IA direction : directions_of(m.first)) {
            const std::string name = control_name(direction, glyphs, bindings, language);
            if (name.empty()) return {};
            if (!list.empty()) list += " / ";
            list += name;
        }
        return out + list;
    }
    const std::string first = one(m.first);
    if (first.empty()) return {};
    if (m.second == IA::count) return out + first;
    // Two directions of one stick read as one: "Left stick left / right".
    if (!keys && glyphs != Glyphs::touch && stick(m.first) && stick(m.second) &&
        left_stick(m.first) == left_stick(m.second))
        return out + stick_name(m.first, language) + " " + direction_name(m.first, language) + " / " +
               direction_name(m.second, language);
    const std::string second = one(m.second);
    if (second.empty()) return {};
    if (glyphs == Glyphs::touch && stick(m.first) && stick(m.second) && left_stick(m.first) == left_stick(m.second))
        return out + control_name(m.first, glyphs, bindings, language) + " " + direction_name(m.first, language) + " / " +
               direction_name(m.second, language);
    // The wipe moves the right stick any way while RT is held.
    if (m.join == '+' && stick(m.second) && !keys)
        return out + first + " + " + stick_name(m.second, language);
    if (m.join == '+' && stick(m.second)) {
        const std::string other = control_name(IA::right_stick_right, glyphs, bindings, language);
        if (other.empty()) return {};
        return out + first + " + " + second + " / " + other;
    }
    return out + first + (m.join == '+' ? " + " : " / ") + second;
}

// --- pieces --------------------------------------------------------------------

namespace {
constexpr uint32_t face_colours[4] = {0x3CB043, 0xE0453A, 0x2F78E0, 0xF2C21B};  // A B X Y
constexpr uint32_t ps_colours[4] = {0x86A8F0, 0xF07070, 0xE79BD8, 0x46D1B0};    // cross circle square triangle

std::string upper(std::string text) {
    for (char& c : text)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return text;
}

// A key's name as drawn: short, upper case, the arrow keys as arrows.
std::string key_label(int key) {
    switch (key) {
    case 0x25: return "\x01";
    case 0x27: return "\x02";
    case 0x26: return "\x03";
    case 0x28: return "\x04";
    case 0x0D: return "ENTER";
    case 0x20: return "SPACE";
    case 0x1B: return "ESC";
    case 0x08: return "BKSP";
    case 0x09: return "TAB";
    case 0x10: return "SHIFT";
    case 0x11: return "CTRL";
    case 0x12: return "ALT";
    }
    std::string name = upper(key_name(key));
    if (name.rfind("NUMPAD ", 0) == 0) name = "NUM" + name.substr(7);
    if (name.rfind("PAGE ", 0) == 0) name = "PG" + name.substr(5);
    return name;
}

// A pad button's picture: `source` is the pad's own control.
Piece pad_piece(IA source, Glyphs glyphs) {
    const bool ps = glyphs == Glyphs::playstation;
    if (source <= IA::y) {
        const size_t i = size_t(source);
        if (ps) return {Shape(size_t(Shape::cross) + i), "", ps_colours[i], 0};
        static const char* letters[4] = {"A", "B", "X", "Y"};
        return {Shape::face, letters[i], face_colours[i], 0};
    }
    if (dpad(source)) return {Shape::dpad, "", 0, arrow_of(source)};
    if (stick(source)) return {Shape::stick, left_stick(source) ? "L" : "R", 0, arrow_of(source)};
    return {Shape::bumper, upper(control_name(source, glyphs, InputBindings{})), 0, 0};
}

std::vector<Piece> control_pieces(IA control, Glyphs glyphs, const InputBindings& bindings, bool every_way) {
    switch (glyphs) {
    case Glyphs::keyboard: {
        if (every_way && (stick(control) || dpad(control))) {
            std::vector<Piece> keys;
            for (const IA direction : directions_of(control)) {
                const int key = bindings[direction];
                if (!key) return {};
                keys.push_back({Shape::key, key_label(key), 0, 0});
            }
            return keys;
        }
        const int key = bindings[control];
        if (!key) return {};
        return {{Shape::key, key_label(key), 0, 0}};
    }
    case Glyphs::touch: {
        if (stick(control) || dpad(control)) {
            const bool right = right_stick(control);
            return {{Shape::stick, right ? "R" : "L", 0, every_way ? uint8_t(0) : arrow_of(control)}};
        }
        const std::string name = touch_name(control);
        if (name.empty()) return {};
        if (control <= IA::y) return {{Shape::face, name, face_colours[size_t(control)], 0}};
        return {{Shape::bumper, name, 0, 0}};
    }
    case Glyphs::xbox:
    case Glyphs::playstation: {
        const IA source = stick(control) ? control : pad_source(control, bindings);
        if (size_t(source) >= input_action_count) return {};
        Piece piece = pad_piece(source, glyphs);
        if (every_way && (piece.shape == Shape::stick || piece.shape == Shape::dpad)) piece.arrows = 0;
        return {piece};
    }
    }
    return {};
}
}  // namespace

std::vector<Piece> prompt_pieces(Action action, Glyphs glyphs, const InputBindings& bindings) {
    if (size_t(action) >= action_count) return {};
    const Mapping m = mapping(action);
    std::vector<Piece> out;
    static const char* verbs[5] = {"", "HOLD", "RELEASE", "", "SPIN"};
    if (*verbs[size_t(m.verb)]) out.push_back({Shape::word, verbs[size_t(m.verb)], 0, 0});
    const bool every_way = all_directions(action);
    std::vector<Piece> first = control_pieces(m.first, glyphs, bindings, every_way);
    if (first.empty()) return {};
    if (m.second != IA::count && glyphs != Glyphs::keyboard && stick(m.first) && stick(m.second) &&
        left_stick(m.first) == left_stick(m.second)) {
        // Two ways of one stick: one stick, both arrows.
        first.front().arrows = uint8_t(arrow_of(m.first) | arrow_of(m.second));
        out.insert(out.end(), first.begin(), first.end());
        return out;
    }
    out.insert(out.end(), first.begin(), first.end());
    if (m.second == IA::count) return out;
    // The wipe: RT and the right stick moved any way.
    std::vector<Piece> second = control_pieces(m.second, glyphs, bindings, m.join == '+');
    if (second.empty()) return {};
    if (m.join == '+' && glyphs == Glyphs::keyboard) {
        // RT and any of the right stick's keys: the two of them that stand for it here.
        second.clear();
        for (const IA way : {IA::right_stick_left, IA::right_stick_right}) {
            const int key = bindings[way];
            if (!key) return {};
            second.push_back({Shape::key, key_label(key), 0, 0});
        }
        out.push_back({Shape::word, "+", 0, 0});
        out.push_back(second[0]);
        out.push_back({Shape::word, "/", 0, 0});
        out.push_back(second[1]);
        return out;
    }
    out.push_back({Shape::word, m.join == '+' ? "+" : "/", 0, 0});
    out.insert(out.end(), second.begin(), second.end());
    return out;
}

// --- drawing -------------------------------------------------------------------

namespace {
// 5x7 letters, five columns each, bit 0 the top row (as native_presentation.cpp's).
const uint8_t* glyph(char c) {
    static constexpr uint8_t letters[26][5] = {
        {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
        {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
        {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
        {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
        {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
        {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
        {7,8,112,8,7},{97,81,73,69,67}};
    static constexpr uint8_t digits[10][5] = {
        {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
        {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
        {54,73,73,73,54},{6,73,73,41,30}};
    static constexpr uint8_t arrows[4][5] = {
        {8,28,62,8,8},     // left
        {8,8,62,28,8},     // right
        {4,6,127,6,4},     // up
        {16,48,127,48,16}}; // down
    static constexpr uint8_t slash[5] = {64,48,8,6,1}, plus[5] = {8,8,62,8,8}, minus[5] = {8,8,8,8,8};
    static constexpr uint8_t comma[5] = {0,80,48,0,0}, period[5] = {0,96,96,0,0}, semicolon[5] = {0,86,54,0,0};
    static constexpr uint8_t equals[5] = {20,20,20,20,20}, backslash[5] = {1,6,8,48,64};
    static constexpr uint8_t left_bracket[5] = {0,127,65,65,0}, right_bracket[5] = {0,65,65,127,0};
    static constexpr uint8_t quote[5] = {0,0,7,0,0}, grave[5] = {0,1,2,4,0}, star[5] = {20,8,62,8,20};
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 1 && c <= 4) return arrows[c - 1];
    switch (c) {
    case '/': return slash;
    case '+': return plus;
    case '-': return minus;
    case ',': return comma;
    case '.': return period;
    case ';': return semicolon;
    case '=': return equals;
    case '\\': return backslash;
    case '[': return left_bracket;
    case ']': return right_bracket;
    case '\'': return quote;
    case '`': return grave;
    case '*': return star;
    }
    return nullptr;
}

int text_width(const std::string& text) { return text.empty() ? 0 : int(text.size()) * 6 - 1; }

constexpr int row = 11;          // a button's height, in units
constexpr int gap = 3;           // between pieces
constexpr int pad_x = 1, pad_y = 1;  // room for the outline
constexpr uint32_t panel = 0x0C1218, light = 0xEEF3F8, dark = 0x141A22, rim = 0xB8C4D0;
constexpr uint32_t none = 0xFF000000;

// PlayStation shapes, 7x7, bit 0 the left column of each row.
constexpr uint8_t ps_shapes[4][7] = {
    {65,34,20,8,20,34,65},     // cross
    {28,34,65,65,65,34,28},    // circle
    {127,65,65,65,65,65,127},  // square
    {8,8,20,20,34,34,127}};    // triangle

int arrow_count(uint8_t arrows) {
    int n = 0;
    for (const uint8_t bit : {arrow::left, arrow::right, arrow::up, arrow::down}) n += (arrows & bit) ? 1 : 0;
    return n;
}

int piece_width(const Piece& piece) {
    switch (piece.shape) {
    case Shape::word: return text_width(piece.label);
    case Shape::face: case Shape::cross: case Shape::circle: case Shape::square: case Shape::triangle: return row;
    case Shape::dpad: return row + (piece.arrows ? 1 + 6 * arrow_count(piece.arrows) - 1 : 0);
    case Shape::stick: return row + (piece.arrows ? 1 + 6 * arrow_count(piece.arrows) - 1 : 0);
    case Shape::bumper: return text_width(piece.label) + 4;
    case Shape::key: return (std::max)(row, text_width(piece.label) + 4);
    }
    return 0;
}

struct Canvas {
    int width, height;
    std::vector<uint32_t> pixels;
    Canvas(int w, int h) : width(w), height(h), pixels(size_t(w) * size_t(h), none) {}
    void set(int x, int y, uint32_t colour) {
        if (x >= 0 && y >= 0 && x < width && y < height) pixels[size_t(y) * size_t(width) + size_t(x)] = colour;
    }
    void fill(int x, int y, int w, int h, uint32_t colour) {
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) set(x + i, y + j, colour);
    }
    // A rounded box: the corner pixels left out.
    void box(int x, int y, int w, int h, uint32_t colour) {
        fill(x, y, w, h, colour);
        set(x, y, none); set(x + w - 1, y, none); set(x, y + h - 1, none); set(x + w - 1, y + h - 1, none);
    }
    void disc(int x, int y, int size, uint32_t colour) {
        const float r = size * 0.5f;
        for (int j = 0; j < size; ++j)
            for (int i = 0; i < size; ++i) {
                const float dx = i + 0.5f - r, dy = j + 0.5f - r;
                if (dx * dx + dy * dy <= r * r + 0.5f) set(x + i, y + j, colour);
            }
    }
    void text(int x, int y, const std::string& label, uint32_t colour) {
        for (const char c : label) {
            if (const uint8_t* columns = glyph(c))
                for (int col = 0; col < 5; ++col)
                    for (int r = 0; r < 7; ++r)
                        if (columns[col] & (1 << r)) set(x + col, y + r, colour);
            x += 6;
        }
    }
    std::string arrows_text(uint8_t arrows) const {
        std::string out;
        if (arrows & arrow::left) out += '\x01';
        if (arrows & arrow::right) out += '\x02';
        if (arrows & arrow::up) out += '\x03';
        if (arrows & arrow::down) out += '\x04';
        return out;
    }
    void piece(int x, int y, const Piece& p) {
        switch (p.shape) {
        case Shape::word: text(x, y + 2, p.label, light); break;
        case Shape::face:
            disc(x, y, row, p.colour);
            text(x + 3, y + 2, p.label.substr(0, 1), dark);
            break;
        case Shape::cross: case Shape::circle: case Shape::square: case Shape::triangle: {
            disc(x, y, row, dark);
            const uint8_t* shape = ps_shapes[size_t(p.shape) - size_t(Shape::cross)];
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 7; ++col)
                    if (shape[r] & (1 << col)) set(x + 2 + col, y + 2 + r, p.colour);
            break;
        }
        case Shape::bumper:
            box(x, y + 1, piece_width(p), row - 2, 0x3A4450);
            text(x + 2, y + 2, p.label, light);
            break;
        case Shape::key: {
            const int w = piece_width(p);
            box(x, y, w, row, light);
            fill(x + 1, y + row - 2, w - 2, 1, rim);  // the key's lower edge
            text(x + (w - text_width(p.label)) / 2, y + 2, p.label, dark);
            break;
        }
        case Shape::stick:
            disc(x, y, row, rim);
            disc(x + 1, y + 1, row - 2, dark);
            text(x + 3, y + 2, p.label.substr(0, 1), light);
            if (p.arrows) text(x + row + 1, y + 2, arrows_text(p.arrows), light);
            break;
        case Shape::dpad:
            box(x + 3, y, 5, row, 0x3A4450);
            box(x, y + 3, row, 5, 0x3A4450);
            fill(x + 5, y + 5, 1, 1, rim);
            if (p.arrows) text(x + row + 1, y + 2, arrows_text(p.arrows), light);
            break;
        }
    }
};
}  // namespace

Size prompt_size(const std::vector<Piece>& pieces) {
    if (pieces.empty()) return {0, 0};
    int width = 2 * pad_x;
    for (size_t i = 0; i < pieces.size(); ++i) width += piece_width(pieces[i]) + (i ? gap : 0);
    return {width, row + 2 * pad_y};
}

std::vector<Rect> prompt_rectangles(const std::vector<Piece>& pieces, int x, int y, int unit) {
    const Size size = prompt_size(pieces);
    if (!size.width || unit <= 0) return {};
    Canvas canvas(size.width, size.height);
    int at = pad_x;
    for (const Piece& piece : pieces) {
        canvas.piece(at, pad_y, piece);
        at += piece_width(piece) + gap;
    }
    // No panel under the prompt, which would hide the game: a one-unit dark
    // outline around what is drawn keeps it readable on any background.
    const std::vector<uint32_t> drawn = canvas.pixels;
    const auto drawn_at = [&](int i, int j) {
        return i >= 0 && j >= 0 && i < canvas.width && j < canvas.height &&
               drawn[size_t(j) * size_t(canvas.width) + size_t(i)] != none;
    };
    for (int j = 0; j < canvas.height; ++j)
        for (int i = 0; i < canvas.width; ++i)
            if (!drawn_at(i, j))
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                        if (drawn_at(i + di, j + dj)) canvas.set(i, j, panel);
    std::vector<Rect> out;
    for (int j = 0; j < canvas.height; ++j)
        for (int i = 0; i < canvas.width;) {
            const uint32_t colour = canvas.pixels[size_t(j) * size_t(canvas.width) + size_t(i)];
            int end = i + 1;
            while (end < canvas.width && canvas.pixels[size_t(j) * size_t(canvas.width) + size_t(end)] == colour) ++end;
            if (colour != none) out.push_back({x + i * unit, y + j * unit, x + end * unit, y + (j + 1) * unit, colour});
            i = end;
        }
    return out;
}

// --- the game's pose figures -------------------------------------------------------

namespace {
std::mutex sightings_mutex;
std::vector<Sighting> sightings_now, sightings_shown;
int sightings_held = 0;
}  // namespace

uint64_t picture_hash(const std::function<const uint8_t*(uint32_t offset)>& sample) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t offset = 0; offset < picture_bytes; offset += picture_step) {
        const uint8_t* bytes = sample(offset);
        if (!bytes) return 0;
        for (uint32_t i = 0; i < picture_sample; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    }
    return hash;
}

namespace {
// The pose flip-books, by picture_hash. Their frames are packed in three rows
// (v 0..0.28, 0.28..0.5625, 0.5625..0.84), four or five a row.
enum Book : uint8_t { start_book, reach_book, lean_book, arm_book, side_book, kick_book, fly_book, book_count };
constexpr uint64_t books[book_count] = {
    0x16C3D38A85972DAEull,  // start: standing sideways, leaning forward to speed up, facing forward to brake
    0x0CF48ECA56F0F998ull,  // reaching to a side, both hands up, crouching and jumping
    0x4542D7607C162002ull,  // leaning forward and back
    0xA27AB9299BB79DAFull,  // the lean back's end, then one arm out to either side
    0x3CDD82CC425B3BBEull,  // seen from the side: crouched, a leg out
    0x8A6C70039E6B3410ull,  // kicking back
    0xD040AD7B08BC7E86ull,  // the T-pose
};
constexpr float row1 = 0.28f, row2 = 0.5625f, row3 = 0.85f;
// Frames by the centre of the texture coordinates the draw samples; no_prompt
// (Action::count) is a pose that asks for nothing on a pad (standing
// sideways accelerates by itself), which ends the pose before it. Frames
// not listed are between poses and keep the last one.
constexpr Action no_prompt = Action::count;
struct Frames { Book book; float u0, v0, u1, v1; Action action; };
constexpr Frames frames[] = {
    // Start (the tutorial's first lesson, traced): the side view says nothing
    // to do; leaning forward is the board's acceleration (the left stick
    // forward, nui_race_hooks.cpp); facing forward is the brake.
    {start_book, 0.00f, 0.0f, 0.18f, row1, no_prompt},
    {start_book, 0.18f, 0.0f, 0.54f, row1, Action::brake_grab},
    {start_book, 0.54f, 0.0f, 1.00f, row1, no_prompt},
    {start_book, 0.00f, row1, 0.36f, row2, no_prompt},
    {start_book, 0.36f, row1, 1.00f, row2, Action::accelerate},
    {start_book, 0.00f, row2, 0.36f, row3, Action::accelerate},
    {start_book, 0.36f, row2, 0.54f, row3, no_prompt},
    {start_book, 0.54f, row2, 1.00f, row3, Action::brake_grab},
    // An arm out to a side, then both hands up; crouching, then the jump.
    {reach_book, 0.00f, 0.0f, 0.69f, row1, Action::reach},
    {reach_book, 0.69f, 0.0f, 1.00f, row1, Action::raise_hands},
    {reach_book, 0.00f, row1, 1.00f, row2, Action::raise_hands},
    {reach_book, 0.00f, row2, 0.18f, row3, Action::crouch},
    {reach_book, 0.18f, row2, 0.54f, row3, Action::jump},
    // Leaning (the tutorial's curving lesson, traced): the steering.
    {lean_book, 0.56f, 0.0f, 1.00f, row1, Action::steer},
    {lean_book, 0.00f, row1, 1.00f, row3, Action::steer},
    // The lean back goes on into the first two frames here (traced). Then
    // one arm stretched out to a side, the other way on the next row: the
    // rings beside the board.
    {arm_book, 0.00f, 0.0f, 0.40f, row1, Action::steer},
    {arm_book, 0.00f, row1, 1.00f, row2, Action::reach},
    {arm_book, 0.18f, row2, 1.00f, row3, Action::reach},
    // Kicking a leg back: the kick dash.
    {kick_book, 0.00f, 0.0f, 1.00f, row2, Action::kick_dash},
    // Arms spread: the fly skill.
    {fly_book, 0.30f, 0.0f, 1.00f, row1, Action::fly},
    {fly_book, 0.00f, row1, 0.62f, row2, Action::fly},
};

std::optional<Book> book_of(uint64_t picture) {
    if (!picture) return std::nullopt;
    for (uint8_t book = 0; book < book_count; ++book)
        if (books[book] == picture) return Book(book);
    return std::nullopt;
}
}  // namespace

bool pose_picture(uint64_t picture) { return book_of(picture).has_value(); }

std::optional<Action> guide_action(uint64_t picture, float u, float v) {
    const auto book = book_of(picture);
    if (!book) return std::nullopt;
    for (const Frames& f : frames)
        if (f.book == *book && u >= f.u0 && u < f.u1 && v >= f.v0 && v < f.v1) return f.action;
    return std::nullopt;
}

void note_sighting(const Sighting& sighting) {
    std::lock_guard lock(sightings_mutex);
    if (sightings_now.size() < 8) sightings_now.push_back(sighting);
}

std::vector<Sighting> take_sightings(int hold) {
    std::lock_guard lock(sightings_mutex);
    if (!sightings_now.empty()) {
        sightings_shown.swap(sightings_now);
        sightings_now.clear();
        sightings_held = 0;
    } else if (++sightings_held > hold) {
        sightings_shown.clear();
    }
    return sightings_shown;
}

Placement place_prompt(const Sighting& s, Size size, const float view[4], const float area[2], int window_width,
                       int window_height) {
    // About 1/250 of the shown picture's height a unit: 3 pixels at 720
    // lines, the prompt's 15 units then 6% of the height.
    const int unit = (std::max)(1, int(std::lround(view[3] / 250.0f)));
    const float ax = area[0] > 0 ? area[0] : 1.0f, ay = area[1] > 0 ? area[1] : 1.0f;
    const float centre = view[0] + (s.left + s.right) * 0.5f / ax * view[2];
    const float bottom = view[1] + s.bottom / ay * view[3];
    const float height = (s.bottom - s.top) / ay * view[3];
    const int width_px = size.width * unit, height_px = size.height * unit;
    int x = int(std::lround(centre - width_px * 0.5f));
    int y = int(std::lround(bottom - height * 0.06f));
    x = std::clamp(x, 0, (std::max)(0, window_width - width_px));
    y = std::clamp(y, 0, (std::max)(0, window_height - height_px));
    return {x, y, unit};
}

// --- the players' input ----------------------------------------------------------

namespace {
struct PlayerInput {
    std::atomic<uint8_t> last{0xFF};      // Glyphs, or 0xFF before anything is used
    std::atomic<uint8_t> pad_kind{0xFF};  // the connected pad's Glyphs, or 0xFF
};
std::array<PlayerInput, 4> players;
std::mutex bindings_mutex;
std::array<std::shared_ptr<const InputBindings>, 4> player_bindings;
}  // namespace

namespace {
// SFR_BUTTON_PROMPTS: 0 off, 1 (or unset) on; a glyph set's name
// (xbox, playstation, keyboard, touch) shows that one whatever is used, a
// test aid for screenshots without the device at hand.
const char* prompt_setting() {
    static const char* text = std::getenv("SFR_BUTTON_PROMPTS");
    return text ? text : "";
}
std::optional<Glyphs> forced_glyphs() {
    const std::string_view text = prompt_setting();
    if (text == "xbox") return Glyphs::xbox;
    if (text == "playstation") return Glyphs::playstation;
    if (text == "keyboard") return Glyphs::keyboard;
    if (text == "touch") return Glyphs::touch;
    return std::nullopt;
}
}  // namespace

bool enabled() { return *prompt_setting() != '0'; }

void set_bindings(uint32_t player, std::shared_ptr<const InputBindings> bindings) {
    if (player >= players.size()) return;
    std::lock_guard lock(bindings_mutex);
    player_bindings[player] = std::move(bindings);
}

std::shared_ptr<const InputBindings> bindings(uint32_t player) {
    if (player >= players.size()) return {};
    std::lock_guard lock(bindings_mutex);
    if (!player_bindings[player]) player_bindings[player] = std::make_shared<InputBindings>(default_bindings(player));
    return player_bindings[player];
}

void note_device(uint32_t player, Glyphs glyphs) {
    if (player < players.size()) players[player].last.store(uint8_t(glyphs), std::memory_order_relaxed);
}

void note_pad_used(uint32_t player) {
    if (player >= players.size()) return;
    const uint8_t pad = players[player].pad_kind.load(std::memory_order_relaxed);
    players[player].last.store(pad != 0xFF ? pad : uint8_t(Glyphs::xbox), std::memory_order_relaxed);
}

void note_pad_present(uint32_t player, bool present, Glyphs kind) {
    if (player < players.size()) players[player].pad_kind.store(present ? uint8_t(kind) : 0xFF, std::memory_order_relaxed);
}

Glyphs device(uint32_t player, bool touch_shown) {
    if (const auto forced = forced_glyphs()) return *forced;
    if (player >= players.size()) return Glyphs::keyboard;
    const uint8_t last = players[player].last.load(std::memory_order_relaxed);
    const uint8_t pad = players[player].pad_kind.load(std::memory_order_relaxed);
    // On-screen buttons that are shown are what a phone player holds; a pad's
    // input hides them (touch_controls.h).
    if (touch_shown && player == 0) return Glyphs::touch;
    if (last != 0xFF && Glyphs(last) != Glyphs::touch) {
        // A pad of the other kind plugged in since: name what is held now.
        if ((Glyphs(last) == Glyphs::xbox || Glyphs(last) == Glyphs::playstation) && pad != 0xFF) return Glyphs(pad);
        return Glyphs(last);
    }
    if (pad != 0xFF) return Glyphs(pad);
    return Glyphs::keyboard;
}

}  // namespace sfr::prompts
