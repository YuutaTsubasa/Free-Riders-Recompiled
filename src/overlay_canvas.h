#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {
struct DecodedImage;

// A colour, straight (not premultiplied) 0..1.
struct OverlayColor {
    float r = 0, g = 0, b = 0, a = 1;
};
OverlayColor overlay_rgb(uint32_t rgb, float alpha = 1.0f);

// Text faces from the host's own font files (nothing is shipped): a regular
// and a bold Latin face, and a CJK face for Japanese names. Missing faces
// fall back to the others; with none, text is not drawn at all.
class OverlayFonts {
public:
    enum Face : int { regular = 0, bold = 1 };
    OverlayFonts();
    ~OverlayFonts();
    OverlayFonts(const OverlayFonts&) = delete;
    OverlayFonts& operator=(const OverlayFonts&) = delete;
    // Loads the first readable file of each list (empty lists: the host's
    // usual files, overlay_font_candidates()).
    void load(const std::vector<std::filesystem::path>& regular_files, const std::vector<std::filesystem::path>& bold_files,
              const std::vector<std::filesystem::path>& cjk_files);
    bool any() const;
    // Advance width of `text` at `pixels` (the em height), in pixels.
    float measure(std::string_view text, float pixels, Face face) const;
    struct Impl;
    Impl& impl() const { return *impl_; }
private:
    std::unique_ptr<Impl> impl_;
};
struct OverlayFontCandidates {
    std::vector<std::filesystem::path> regular, bold, cjk;
};
OverlayFontCandidates overlay_font_candidates();

// An RGBA picture drawn in software, premultiplied by alpha (screen_overlay.h
// takes it as it is). Shapes are anti-aliased by coverage.
class OverlayCanvas {
public:
    OverlayCanvas(uint32_t width, uint32_t height);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::vector<uint8_t>& rgba() const { return rgba_; }
    std::vector<uint8_t> take() { return std::move(rgba_); }

    // A rounded rectangle filled top to bottom from `top` to `bottom`.
    void fill_round_rect(float x, float y, float w, float h, float radius, OverlayColor top, OverlayColor bottom);
    void fill_round_rect(float x, float y, float w, float h, float radius, OverlayColor color) {
        fill_round_rect(x, y, w, h, radius, color, color);
    }
    // Its outline, `thickness` wide inside the edge.
    void stroke_round_rect(float x, float y, float w, float h, float radius, float thickness, OverlayColor color);
    void fill_circle(float cx, float cy, float radius, OverlayColor color);
    // A picture scaled into the rectangle (bilinear), corners rounded.
    void draw_image(const DecodedImage& image, float x, float y, float w, float h, float radius = 0.0f, float opacity = 1.0f,
                    bool grey = false);
    // Text on a baseline; longer than max_width (if > 0) ends in "...".
    // Returns the width drawn.
    float text(const OverlayFonts& fonts, std::string_view text, float x, float baseline, float pixels, OverlayColor color,
               OverlayFonts::Face face = OverlayFonts::regular, float max_width = 0.0f);

private:
    void blend(int x, int y, OverlayColor color, float coverage);
    uint32_t width_, height_;
    std::vector<uint8_t> rgba_;
};

// Text split into lines no wider than max_width (at spaces; CJK text, which
// has none, anywhere), at most max_lines (the last one ends in "..." when
// text is left over).
std::vector<std::string> wrap_overlay_text(const OverlayFonts& fonts, std::string_view text, float pixels,
                                           OverlayFonts::Face face, float max_width, size_t max_lines);
}
