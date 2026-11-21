// The touch layout editor's settings (Issue #443, touch_layout.h): reading
// and writing touch_layout=, where a placed control lands in images of
// several shapes, and the limits that keep it whole and big enough.
#include "touch_controls.h"
#include "touch_layout.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
bool near(float a, float b, float within = 0.0005f) { return std::abs(a - b) <= within; }
namespace layout = sfr::touch_layout;
using sfr::TouchControls;

constexpr float wide_16x9 = 16.0f / 9.0f, phone_19_5x9 = 19.5f / 9.0f, phone_20x9 = 20.0f / 9.0f;
constexpr float aspects[] = {wide_16x9, phone_19_5x9, phone_20x9, 4.0f / 3.0f};

void numbers() {
    require(layout::parse_number("0.25") == 0.25f && layout::parse_number("-1.5") == -1.5f &&
                layout::parse_number(" 2 ") == 2.0f && layout::parse_number("+.5") == 0.5f,
            "plain decimals read");
    require(!layout::parse_number("") && !layout::parse_number("1,5") && !layout::parse_number("1.2.3") &&
                !layout::parse_number("abc") && !layout::parse_number("-") && !layout::parse_number("1e3") &&
                !layout::parse_number("99999"),
            "anything else does not");
    require(layout::format_number(0.25f, 3) == "0.250" && layout::format_number(-0.0625f, 3) == "-0.063" &&
                layout::format_number(1.4f, 2) == "1.40" && layout::format_number(-0.0001f, 3) == "0.000" &&
                layout::format_number(12.0f, 0) == "12",
            "numbers are written with a point and fixed decimals");
}

void parsing() {
    require(layout::parse("").empty() && layout::format({}).empty(), "empty is the default layout");
    const auto read = layout::parse("ls:l0.320,0.550,1.40;a:r0.276,0.820,0.80;start:c-0.100,0.080,1.00");
    require(read.places[TouchControls::stick] == layout::Place{'l', 0.32f, 0.55f, 1.4f}, "LS reads");
    require(read.places[TouchControls::a] == layout::Place{'r', 0.276f, 0.82f, 0.8f}, "A reads");
    require(read.places[TouchControls::start] == layout::Place{'c', -0.1f, 0.08f, 1.0f}, "START reads");
    require(!read.places[TouchControls::b] && !read.places[TouchControls::right_stick], "the others stay default");
    // Written in names' order, and read back the same.
    const std::string written = layout::format(read);
    require(written == "ls:l0.320,0.550,1.40;a:r0.276,0.820,0.80;start:c-0.100,0.080,1.00", "the layout writes back");
    require(layout::parse(written) == read, "and survives a round trip");
    // Hand edits: any case, spaces, unknown names and broken entries.
    const auto edited = layout::parse(" A : R0.3, 0.5 ,1 ;nonsense:l1,1,1;b:x0.1,0.1,1;x:l0.1,0.1;y:l0.1,abc,1;"
                                      "rt:l0.1,0.2,1.0,9;lb;;rs:");
    require(edited.places[TouchControls::a] == layout::Place{'r', 0.3f, 0.5f, 1.0f}, "a hand-edited entry reads");
    for (size_t i = 0; i < layout::count; ++i)
        if (i != TouchControls::a) require(!edited.places[i], "a broken or unknown entry is ignored");
    // A size outside the limits is brought within them as it is read.
    const auto sized = layout::parse("a:l1,0.5,9;b:l1,0.5,0.1;start:l1,0.5,0.5");
    require(sized.places[TouchControls::a]->scale == layout::max_scale, "too big is the largest size");
    require(sized.places[TouchControls::b]->scale == layout::min_scale_of(TouchControls::b), "too small is the smallest size");
    require(sized.places[TouchControls::start]->scale == 1.0f, "START, already the smallest, does not shrink");
}

void defaults() {
    // Nothing placed: exactly the layout PR #444 draws in every shape.
    for (const float aspect : aspects)
        for (size_t i = 0; i < layout::count; ++i) {
            const auto resolved = layout::resolve(i, std::nullopt, aspect);
            require(resolved == TouchControls::layout(TouchControls::Button(i), aspect), "no place is the default layout");
        }
    // The defaults read as places give the same circles (the editor's Reset
    // and a saved default agree), in every shape.
    for (const float aspect : aspects)
        for (size_t i = 0; i < layout::count; ++i) {
            const auto [x, y, r] = TouchControls::layout(TouchControls::Button(i), aspect);
            const auto place = layout::place_at(i, x, y, 1.0f, aspect);
            const auto again = layout::resolve(i, place, aspect);
            require(near(again[0], x, 0.001f) && near(again[1], y) && again[2] == r, "a default read as a place stays");
        }
}

void shapes() {
    // LS bigger and higher, A smaller, START off centre: made on a 16:9 tablet.
    const auto made = layout::parse("ls:l0.320,0.550,1.40;a:r0.276,0.820,0.80;start:c-0.100,0.080,1.00");
    struct Expect { float aspect, ls_x, a_x, start_x; };
    // l: 0.320 heights from the left; r: 0.276 from the right; c: 0.1 left of the centre.
    const Expect expect[] = {
        {wide_16x9, 0.320f / wide_16x9, 1.0f - 0.276f / wide_16x9, 0.5f - 0.1f / wide_16x9},
        {phone_19_5x9, 0.320f / phone_19_5x9, 1.0f - 0.276f / phone_19_5x9, 0.5f - 0.1f / phone_19_5x9},
        {phone_20x9, 0.320f / phone_20x9, 1.0f - 0.276f / phone_20x9, 0.5f - 0.1f / phone_20x9},
    };
    for (const auto& e : expect) {
        const auto ls = layout::resolve(TouchControls::stick, made.places[TouchControls::stick], e.aspect);
        const auto a = layout::resolve(TouchControls::a, made.places[TouchControls::a], e.aspect);
        const auto start = layout::resolve(TouchControls::start, made.places[TouchControls::start], e.aspect);
        require(near(ls[0], e.ls_x) && near(ls[1], 0.55f) && near(ls[2], 0.13f * 1.4f), "LS keeps its distance from the left");
        require(near(a[0], e.a_x) && near(a[1], 0.82f) && near(a[2], 0.075f * 0.8f), "A keeps its distance from the right");
        require(near(start[0], e.start_x) && near(start[1], 0.08f), "START keeps its distance from the centre");
    }
    // On a 19.5:9 phone, 2340 x 1080 (the reporter's Galaxy A55): A is 0.276
    // image heights (298 pixels) from the right edge, as on the tablet.
    const auto a = layout::resolve(TouchControls::a, made.places[TouchControls::a], phone_19_5x9);
    require(near((1.0f - a[0]) * 2340.0f, 0.276f * 1080.0f, 0.5f), "the same distance in pixels of height");
}

void editing() {
    // The editor picks the nearest anchor: an edge, or the centre.
    require(layout::place_at(TouchControls::a, 0.9f, 0.5f, 1.0f, phone_20x9).anchor == 'r', "near the right: from the right");
    require(layout::place_at(TouchControls::a, 0.1f, 0.5f, 1.0f, phone_20x9).anchor == 'l', "near the left: from the left");
    require(layout::place_at(TouchControls::start, 0.5f, 0.1f, 1.0f, phone_20x9).anchor == 'c', "the middle: from the centre");
    require(layout::place_at(TouchControls::a, 0.45f, 0.5f, 1.0f, wide_16x9).anchor == 'c', "nearer the centre than an edge");
    // A place made on a 20:9 phone lands where it was put there.
    for (const float aspect : aspects) {
        const float xs[] = {0.12f, 0.3f, 0.47f, 0.5f, 0.62f, 0.88f};
        for (const float x : xs) {
            const auto place = layout::place_at(TouchControls::b, x, 0.4f, 1.3f, aspect);
            const auto [rx, ry, rr] = layout::resolve(TouchControls::b, place, aspect);
            require(near(rx, x, 0.001f) && near(ry, 0.4f) && near(rr, 0.075f * 1.3f), "a placed control lands where it was put");
        }
    }
    // The values are those that are written.
    const auto place = layout::place_at(TouchControls::y, 0.81234f, 0.43219f, 1.2345f, phone_20x9);
    require(layout::parse(layout::format([&] {
                layout::Layout l;
                l.places[TouchControls::y] = place;
                return l;
            }())).places[TouchControls::y] == place,
            "what the editor shows is what is saved");
}

void stick_area() {
    // The default LS keeps exactly the left 40%, in any shape (on a square
    // screen its circle and slack reach past it, as before the editor).
    for (const float aspect : {wide_16x9, phone_20x9, 1.0f})
        require(layout::race_stick_area(std::nullopt, aspect) == std::array<float, 2>{0.0f, layout::stick_side},
                "the default stick side is the left 40%");
    // A placed LS: its side, reaching its circle.
    const auto right = layout::race_stick_area(layout::place_at(0, 0.8f, 0.5f, 1.0f, wide_16x9), wide_16x9);
    require(right[0] == 1.0f - layout::stick_side && right[1] == 1.0f, "LS on the right: the right 40%");
    const auto inner = layout::race_stick_area(layout::place_at(0, 0.42f, 0.5f, 1.0f, wide_16x9), wide_16x9);
    require(inner[0] == 0.0f && inner[1] > 0.5f, "LS past 40%: the side reaches its circle");
}

void limits() {
    // Off the image, too big, too small: kept whole on it, within the sizes.
    for (const float aspect : aspects) {
        const auto big = layout::resolve(TouchControls::a, layout::Place{'l', -3.0f, 4.0f, 9.0f}, aspect);
        require(near(big[2], 0.075f * layout::max_scale) && near(big[0], big[2] / aspect) && near(big[1], 1.0f - big[2]),
                "a control past the bottom left is kept whole in that corner");
        const auto far = layout::resolve(TouchControls::b, layout::Place{'r', -2.0f, -1.0f, 1.0f}, aspect);
        require(near(far[0], 1.0f - far[2] / aspect) && near(far[1], far[2]), "and past the top right in that one");
        const auto small = layout::resolve(TouchControls::x, layout::Place{'c', 0.0f, 0.5f, 0.01f}, aspect);
        require(near(small[2], layout::min_radius), "X shrinks only to START's size");
        const auto stick = layout::resolve(TouchControls::stick, layout::Place{'l', 0.3f, 0.5f, 0.01f}, aspect);
        require(near(stick[2], 0.13f * layout::min_scale), "the stick shrinks to half");
        const auto lt = layout::resolve(TouchControls::lt, layout::Place{'c', 0.0f, 0.5f, 0.01f}, aspect);
        require(near(lt[2], layout::min_radius), "LT shrinks only to START's size");
        const auto broken = layout::resolve(TouchControls::y, layout::Place{'c', NAN, NAN, NAN}, aspect);
        require(std::isfinite(broken[0]) && std::isfinite(broken[1]) && broken[2] == 0.075f, "nonsense is not drawn as nonsense");
    }
    // The editor's place is kept whole too.
    const auto placed = layout::place_at(TouchControls::stick, -1.0f, 2.0f, 2.0f, phone_20x9);
    const auto [x, y, r] = layout::resolve(TouchControls::stick, placed, phone_20x9);
    require(near(x, r / phone_20x9, 0.001f) && near(y, 1.0f - r, 0.001f) && near(r, 0.26f), "a dragged-off stick stays on screen");
    require(layout::min_scale_of(TouchControls::start) == 1.0f && near(layout::min_scale_of(TouchControls::lt), 0.05f / 0.055f) &&
                near(layout::min_scale_of(TouchControls::a), 0.05f / 0.075f) &&
                layout::min_scale_of(TouchControls::stick) == layout::min_scale,
            "each control's smallest size");
}
}

int main() {
    try {
        numbers();
        parsing();
        defaults();
        shapes();
        editing();
        stick_area();
        limits();
        std::cout << "Touch layout checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
