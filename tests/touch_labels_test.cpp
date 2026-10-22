// The on-screen buttons' names (Issue #236): the glyphs, where each label
// sits on its button, and that src/shaders/touch_labels.hlsli is what the
// generator writes now.
//   sfr_touch_labels_test <touch_labels.hlsli>          checks
//   sfr_touch_labels_test --write <touch_labels.hlsli>  rewrites the include
#include "touch_controls.h"
#include "touch_labels.h"
#include "fps_readout.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const std::string& detail) { if (!ok) throw std::runtime_error(detail); }
using sfr::TouchControls;

// The text with its numbers taken out (each replaced by '#'), and the numbers.
std::pair<std::string, std::vector<double>> split_numbers(const std::string& text) {
    std::string skeleton;
    std::vector<double> numbers;
    for (size_t i = 0; i < text.size();) {
        const bool sign = text[i] == '-' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]));
        const bool word = i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_');
        if ((sign || std::isdigit(static_cast<unsigned char>(text[i]))) && !word) {
            char* end = nullptr;
            numbers.push_back(std::strtod(text.c_str() + i, &end));
            i = size_t(end - text.c_str());
            // A float suffix or exponent belongs to the number.
            skeleton += '#';
            continue;
        }
        skeleton += text[i++];
    }
    return {skeleton, numbers};
}

// Whether a pixel of a w x h raster of the label (one button diameter
// across) is inside a stroke.
std::vector<bool> raster(const sfr::TouchLabel& label, int size) {
    std::vector<bool> pixels(size_t(size * size));
    for (int row = 0; row < size; ++row)
        for (int column = 0; column < size; ++column) {
            const float x = (float(column) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float y = (float(row) + 0.5f) / float(size) * 2.0f - 1.0f;
            pixels[size_t(row * size + column)] = sfr::touch_label_distance(label, x, y) <= 0.0f;
        }
    return pixels;
}

void check_glyphs() {
    for (char letter : std::string("ABLRSTXYFP0123456789")) {
        const auto strokes = sfr::glyph_strokes(letter);
        require(!strokes.empty(), std::string("glyph ") + letter + " has strokes");
        const float width = sfr::glyph_width(letter);
        require(width > 0.3f && width <= 0.7f, std::string("glyph ") + letter + " has a letter's width");
        float top = 1.0f, bottom = 0.0f;
        for (const auto& s : strokes) {
            for (const auto [x, y] : {std::pair{s.x0, s.y0}, std::pair{s.x1, s.y1}}) {
                require(x >= 0.0f && x <= width + 1e-6f && y >= 0.0f && y <= 1.0f,
                        std::string("glyph ") + letter + " stays in its box");
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
            require(std::hypot(s.x1 - s.x0, s.y1 - s.y0) > 0.05f, std::string("glyph ") + letter + " has no dot strokes");
        }
        require(top == 0.0f && bottom == 1.0f, std::string("glyph ") + letter + " spans the cap height");
    }
    require(sfr::glyph_strokes('Q').empty() && sfr::glyph_width('Q') == 0.0f, "an unknown letter has no glyph");
}

void check_labels() {
    const char* expected[TouchControls::right_stick + 1] = {"LS", "A", "B", "X", "Y", "RT", "START", "LB", "RB", "LT", "RS"};
    for (size_t i = 0; i < sfr::TouchOverlay::circles; ++i) {
        const sfr::TouchLabel label = sfr::touch_label(i);
        const std::string name(label.text);
        require(name == expected[i], "circle " + std::to_string(i) + " is named " + expected[i]);
        size_t strokes = 0;
        for (char letter : name) strokes += sfr::glyph_strokes(letter).size();
        require(label.strokes.size() == strokes, name + ": every letter is drawn");
        require(label.on_knob == (i == TouchControls::stick || i == TouchControls::right_stick),
                name + ": only the sticks' names ride on the knob");

        // Inside the button's bright rim (blit.hlsl draws the core to 0.82
        // of the radius) or the knob (0.45), with room for the round ends.
        const float limit = label.on_knob ? 0.45f : 0.82f;
        float left = 1.0f, right = -1.0f, lowest = -1.0f;
        for (const auto& s : label.strokes)
            for (const auto [x, y] : {std::pair{s.x0, s.y0}, std::pair{s.x1, s.y1}}) {
                require(std::hypot(x, y) + label.half_width <= limit, name + ": stays inside its button");
                left = std::min(left, x);
                right = std::max(right, x);
                lowest = std::max(lowest, y);
            }
        require(std::abs(left + right) < 1e-4f, name + ": is centred");
        // LB and RB keep the lower half for the punch's arrow (blit.hlsl:
        // arrow scaled 0.6 around y 0.36, its top near y 0.13).
        if (i == TouchControls::lb || i == TouchControls::rb)
            require(lowest + label.half_width < 0.1f, name + ": leaves room for the arrow");

        // Readable on a small phone: the game image 720 pixels high.
        const float radius_pixels = TouchControls::layout(TouchControls::Button(i))[2] * 720.0f;
        require(label.cap_height * radius_pixels >= 12.0f, name + ": letters at least 12 pixels high at 720p");
        require(label.half_width * 2.0f * radius_pixels >= 2.0f, name + ": strokes at least 2 pixels wide at 720p");

        // The distance: inside on a stroke's middle, outside at the rim.
        const auto& s = label.strokes.front();
        require(sfr::touch_label_distance(label, (s.x0 + s.x1) / 2, (s.y0 + s.y1) / 2) < 0.0f, name + ": stroke inside");
        require(sfr::touch_label_distance(label, 0.0f, -0.99f) > 0.0f, name + ": rim outside");
    }

    // Letters of a label do not run into each other: between neighbours a
    // column of the raster is free of ink.
    for (size_t i : {size_t(TouchControls::start), size_t(TouchControls::lb), size_t(TouchControls::trigger)}) {
        const sfr::TouchLabel label = sfr::touch_label(i);
        constexpr int size = 256;
        const auto pixels = raster(label, size);
        int gaps = 0;
        bool ink_before = false, in_gap = false;
        for (int column = 0; column < size; ++column) {
            bool ink = false;
            for (int row = 0; row < size && !ink; ++row) ink = pixels[size_t(row * size + column)];
            if (ink && in_gap && ink_before) ++gaps;
            if (ink) { ink_before = true; in_gap = false; } else if (ink_before) in_gap = true;
        }
        require(gaps == int(label.text.size()) - 1, std::string(label.text) + ": its letters stand apart");
    }

    // The face buttons tell apart even at 32 pixels across: any two differ
    // in a third of the ink of either.
    std::vector<std::vector<bool>> faces;
    for (size_t i = TouchControls::a; i <= TouchControls::y; ++i) faces.push_back(raster(sfr::touch_label(i), 32));
    const auto ink = [](const std::vector<bool>& pixels) { return int(std::count(pixels.begin(), pixels.end(), true)); };
    for (size_t i = 0; i < faces.size(); ++i)
        for (size_t j = i + 1; j < faces.size(); ++j) {
            int differ = 0;
            for (size_t p = 0; p < faces[i].size(); ++p) differ += faces[i][p] != faces[j][p];
            require(ink(faces[i]) > 0 && 3 * differ >= std::max(ink(faces[i]), ink(faces[j])),
                    "face button letters tell apart at 32 pixels");
        }
}

// The frame-rate readout's text (Issue #236).
void check_readout() {
    require(sfr::readout_digits(0) == 1 && sfr::readout_digits(7) == 1 && sfr::readout_digits(60) == 2 &&
                sfr::readout_digits(144) == 3 && sfr::readout_digits(5000) == 3,
            "the readout shows one to three digits");
    for (uint32_t value : {0u, 9u, 60u, 120u, 999u}) {
        const auto strokes = sfr::readout_strokes(value);
        const std::string digits = std::to_string(value);
        size_t expected = 0;
        for (char letter : digits + "FPS") expected += sfr::glyph_strokes(letter).size();
        require(strokes.size() == expected, "the readout draws " + digits + " FPS");
        const float width = sfr::readout_width(value), half = sfr::readout_half_width();
        float right = 0.0f;
        for (const auto& s : strokes)
            for (const auto [x, y] : {std::pair{s.x0, s.y0}, std::pair{s.x1, s.y1}}) {
                require(x >= 0.0f && y >= 0.0f && y <= 1.0f && x <= width + 1e-4f, digits + ": inside its box");
                right = std::max(right, x);
            }
        require(right > width - 0.2f, digits + ": the box ends at the text");
        require(sfr::readout_distance(value, 0.0f, 0.5f) - half < 0.2f, digits + ": starts at the left edge");
    }
    require(sfr::readout_strokes(5000).size() == sfr::readout_strokes(999).size(), "larger values show as 999");

    // Digits tell apart at 16 pixels high (the readout at 720p is 19).
    constexpr int height = 16, width = 10;
    std::vector<std::vector<bool>> digits;
    for (uint32_t digit = 0; digit <= 9; ++digit) {
        std::vector<bool> pixels(size_t(height * width));
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column)
                pixels[size_t(row * width + column)] =
                    sfr::readout_distance(digit, (column + 0.5f) / height - 0.1f, (row + 0.5f) / height * 1.2f - 0.1f) <= 0.0f;
        digits.push_back(pixels);
    }
    for (size_t i = 0; i < digits.size(); ++i)
        for (size_t j = i + 1; j < digits.size(); ++j) {
            int differ = 0;
            for (size_t p = 0; p < digits[i].size(); ++p) differ += digits[i][p] != digits[j][p];
            require(differ >= 6, "digits " + std::to_string(i) + " and " + std::to_string(j) + " tell apart at 16 pixels");
        }

    require(sfr::fps_readout_word(false, 60) == 0 && sfr::fps_readout_word(true, 60) == 0x1003C &&
                sfr::fps_readout_word(true, 0) == 0x10000 && sfr::fps_readout_word(true, 5000) == 0x103E7,
            "the readout's word: bit 16 shown, then the value up to 999");
    require(sfr::fps_readout_word(true, 60, 3) == 0x31003C && sfr::fps_readout_word(true, 60, 99) == 0x141003C &&
                sfr::fps_readout_word(false, 60, 5) == 0 &&
                (sfr::fps_readout_word(true, 999, sfr::fps_inset_max) & sfr::touch_names_hidden_bit) == 0,
            "the inset goes in bits 20 to 24, at most fps_inset_max, clear of the names' bit");
    sfr::TouchOverlay overlay;
    overlay.readout = sfr::fps_readout_word(true, 59);
    const auto words = sfr::pack_touch_overlay(overlay);
    require(words[sfr::touch_readout_word] == 0x1003B && sfr::touch_readout_word == words.size() - 1,
            "the readout goes in the blit's last word");
}

void check_counter() {
    sfr::FpsCounter counter;
    require(counter.value() == 0, "no rate before any frame");
    double t = 100.0;
    for (int i = 0; i < 20; ++i) counter.presented(t += 1.0 / 60.0);
    require(counter.value() == 0, "no rate before half a second has passed");
    for (int i = 0; i < 120; ++i) counter.presented(t += 1.0 / 60.0);
    require(counter.value() == 60, "60 frames a second read 60");
    for (int i = 0; i < 90; ++i) counter.presented(t += 1.0 / 30.0);
    require(counter.value() == 30, "30 frames a second read 30");
    // A long stall (a loading screen): one frame after 2 seconds.
    counter.presented(t += 2.0);
    require(counter.value() < 10, "a stall reads as few frames");
    for (int i = 0; i < 150; ++i) counter.presented(t += 1.0 / 144.0);
    require(counter.value() == 144, "144 frames a second read 144");
    // A clock that goes back starts counting again rather than dividing by it.
    counter.presented(t - 10.0);
    require(counter.value() == 144, "a clock going back keeps the last value");
}

void check_include(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "cannot read " + path);
    std::stringstream contents;
    contents << file.rdbuf();
    std::string have = contents.str();
    have.erase(std::remove(have.begin(), have.end(), '\r'), have.end());
    const auto [want_text, want_numbers] = split_numbers(sfr::touch_labels_hlsl());
    const auto [have_text, have_numbers] = split_numbers(have);
    const std::string stale = path + " is stale: run sfr_touch_labels_test --write " + path;
    require(want_text == have_text && want_numbers.size() == have_numbers.size(), stale);
    for (size_t i = 0; i < want_numbers.size(); ++i)
        require(std::abs(want_numbers[i] - have_numbers[i]) <= 2e-4, stale);
    require(have.find("label_distance(int i, float2 p)") != std::string::npos, "the include defines label_distance");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::strcmp(argv[1], "--write") == 0) {
            std::ofstream(argv[2], std::ios::binary | std::ios::trunc) << sfr::touch_labels_hlsl();
            std::cout << "wrote " << argv[2] << '\n';
            return 0;
        }
        check_glyphs();
        check_labels();
        check_readout();
        check_counter();
        if (argc >= 2) check_include(argv[1]);
        else std::cout << "no include given; skipped the comparison\n";
    } catch (const std::exception& error) {
        std::cerr << "touch_labels_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "touch_labels_test passed\n";
    return 0;
}
