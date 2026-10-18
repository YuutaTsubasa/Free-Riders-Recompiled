#pragma once

#include "launcher_screen_fit.h"

#include <string>
#include <vector>

namespace sfr::launcher {

// The edges of the launcher's window that the Dynamic Island or notch, the
// rounded corners and the home indicator cover, in points (UIKit's safe area).
Insets ios_safe_area();

// The file of a system font by its PostScript name (CoreText's answer), or
// empty when the system has no such font.
std::string ios_font_file(const char* postscript_name);

// A character drawn by CoreText: its coverage (one byte a pixel, top row
// first), advance and offset from the glyph origin at the font's top, as
// ImGui's custom glyphs take them.
struct RenderedGlyph {
    unsigned int codepoint = 0;
    int width = 0, height = 0;
    float advance = 0, x = 0, y = 0;
    std::vector<unsigned char> coverage;
};
// iOS's Chinese face (PingFang TC) is a file ImGui cannot read: CoreText
// draws those characters, at the size of the launcher's Arial of pixel_size.
std::vector<RenderedGlyph> ios_render_glyphs(const std::vector<unsigned int>& codepoints, float pixel_size, bool bold);

}  // namespace sfr::launcher
