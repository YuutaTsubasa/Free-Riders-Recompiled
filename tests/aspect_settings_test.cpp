#include "aspect_settings.h"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

uint32_t width_of(const char* ratio, uint32_t most = 4096) {
    uint32_t num = 0, den = 0;
    require(sfr::aspect::parse_ratio(ratio, num, den), "the ratio parses");
    return sfr::aspect::width_for_ratio(1280, 720, num, den, most);
}

void parsing() {
    uint32_t num = 0, den = 0;
    require(sfr::aspect::parse_ratio("21:9", num, den) && num == 21 && den == 9, "21:9 parses");
    require(sfr::aspect::parse_ratio("64:27", num, den) && num == 64 && den == 27, "64:27 parses");
    for (const char* text : {"", "21", "21:", ":9", "21:0", "0:9", "a:b", "21:9:1", "21/9", " 21:9", "21:9 ", "-21:9",
                             "123456:9"}) {
        uint32_t n = 7, d = 7;
        require(!sfr::aspect::parse_ratio(text, n, d), "an invalid ratio is refused");
        require(n == 7 && d == 7, "an invalid ratio leaves the outputs alone");
    }
    require(!sfr::aspect::parse_ratio(nullptr, num, den), "no text is refused");
}

void widths() {
    require(width_of("32:9") == 2560, "32:9 is 2560 wide at 720");
    require(width_of("48:9") == 3840, "48:9 is 3840 wide at 720");
    require(width_of("21:9") == 1680, "21:9 is 1680 wide at 720");
    require(width_of("64:27") == 1706, "64:27 (2560x1080) is 1706 wide, even");
    require(width_of("43:18") == 1720, "43:18 (3440x1440) is 1720 wide");
    require(width_of("48:9", 2000) == 2000, "never wider than the bound");
    require(width_of("16:9") == 1280, "16:9 keeps the title's width");
    require(width_of("4:3") == 1280 && width_of("1:1") == 1280, "narrower than 16:9 keeps the title's width");
    // Within the rounding of 16:9 (center_width + 8) the title's own buffer is kept.
    require(width_of("1281:720") == 1280, "16:9 within rounding keeps the title's width");
    for (const char* ratio : {"21:9", "32:9", "48:9", "64:27", "43:18", "17:9", "5:2"})
        require(width_of(ratio) % 2 == 0, "logical widths are even");
}

void layout_helpers() {
    require(sfr::aspect::center_width(720) == 1280, "the centered 16:9 area at 720 is 1280");
    require(!sfr::aspect::wider_than_16x9(1280, 720), "the original 1280x720 is not wider than 16:9: no ultrawide path");
    require(!sfr::aspect::wider_than_16x9(2560, 1440) && !sfr::aspect::wider_than_16x9(1920, 1080),
            "a scaled 16:9 frame is not wider than 16:9");
    require(sfr::aspect::wider_than_16x9(1680, 720), "21:9 is wider");
}
}

// The race layout (880 wide on a 16:9 frame) widened by the frame's ratio,
// even, so its two halves are whole pixels.
void race_layout() {
    using sfr::aspect::race_layout_width;
    require(race_layout_width(1280) == 880, "16:9 keeps the 880-wide race layout");
    require(race_layout_width(1706) == 1172, "21:9 (1706 wide) widens it to 1172");
    require(race_layout_width(2560) == 1762, "32:9 (2560 wide) takes two pixels more than double");
    for (uint32_t width = 1282; width <= 4096; width += 2)
        require(race_layout_width(width) / 2 != 880 && race_layout_width(width) / 2 != 1280,
                "a half is never 880 or 1280 wide, which the title's own views are");
    for (uint32_t width : {1300u, 1706u, 2000u, 2560u, 4096u})
        require(race_layout_width(width) % 2 == 0, "the layout is even, so the halves are whole");
}

int main() {
    try {
        parsing();
        widths();
        layout_helpers();
        race_layout();
    } catch (const std::exception& error) {
        std::cerr << "aspect_settings_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "aspect_settings_test passed\n";
}
