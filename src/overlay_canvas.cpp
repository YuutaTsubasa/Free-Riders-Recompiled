#include "overlay_canvas.h"
#include "image_decode.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <tuple>

// Dear ImGui's copy of stb_truetype (public domain), when the checkout has
// it; without it the overlay draws no text.
#if __has_include("imstb_truetype.h")
#define SFR_OVERLAY_TEXT 1
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#else
#define SFR_OVERLAY_TEXT 0
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace sfr {
namespace fs = std::filesystem;

OverlayColor overlay_rgb(uint32_t rgb, float alpha) {
    return {float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f, float(rgb & 0xFF) / 255.0f, alpha};
}

OverlayFontCandidates overlay_font_candidates() {
    OverlayFontCandidates c;
#ifdef _WIN32
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const fs::path fonts = fs::path(windows) / L"Fonts";
    c.regular = {fonts / L"segoeui.ttf", fonts / L"arial.ttf"};
    c.bold = {fonts / L"segoeuib.ttf", fonts / L"seguisb.ttf", fonts / L"arialbd.ttf"};
    c.cjk = {fonts / L"YuGothM.ttc", fonts / L"meiryo.ttc", fonts / L"msgothic.ttc", fonts / L"msjh.ttc"};
#elif defined(__ANDROID__)
    const fs::path fonts = "/system/fonts";
    c.regular = {fonts / "RobotoStatic-Regular.ttf", fonts / "Roboto-Regular.ttf", fonts / "DroidSans.ttf"};
    c.bold = {fonts / "Roboto-Bold.ttf", fonts / "RobotoStatic-Medium.ttf", fonts / "DroidSans-Bold.ttf"};
    c.cjk = {fonts / "NotoSansCJK-Regular.ttc", fonts / "NotoSansJP-Regular.otf", fonts / "DroidSansFallback.ttf"};
#elif defined(__APPLE__)
    const fs::path fonts = "/System/Library/Fonts", extra = fonts / "Supplemental";
    c.regular = {extra / "Arial.ttf", "/System/Library/Fonts/WebFonts/Arial.ttf"};
    c.bold = {extra / "Arial Bold.ttf", "/System/Library/Fonts/WebFonts/ArialBold.ttf"};
    c.cjk = {fonts / "ヒラギノ角ゴシック W3.ttc", fonts / "STHeiti Light.ttc", fonts / "Hiragino Sans GB.ttc"};
#else
    c.regular = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                 "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                 "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/google-noto/NotoSans-Regular.ttf"};
    c.bold = {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
              "/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/google-noto/NotoSans-Bold.ttf"};
    c.cjk = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"};
#endif
    return c;
}

namespace {
// The next code point of UTF-8 text (U+FFFD for a broken sequence).
uint32_t next_code_point(std::string_view text, size_t& at) {
    const auto byte = [&](size_t i) { return uint8_t(text[i]); };
    const uint8_t first = byte(at++);
    if (first < 0x80) return first;
    int extra = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t value = first & (0x3F >> extra);
    for (; extra > 0; --extra) {
        if (at >= text.size() || (byte(at) & 0xC0) != 0x80) return 0xFFFD;
        value = value << 6 | (byte(at++) & 0x3F);
    }
    return value;
}
bool is_cjk(uint32_t c) {
    return (c >= 0x3000 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}
}

struct OverlayFonts::Impl {
    struct Font {
        std::vector<uint8_t> data;
#if SFR_OVERLAY_TEXT
        stbtt_fontinfo info{};
#endif
        bool loaded = false;
    };
    std::array<Font, 3> faces;  // regular, bold, cjk
    struct Glyph {
        int width = 0, height = 0, x = 0, y = 0;  // bitmap and its offset from the pen on the baseline
        float advance = 0;
        std::vector<uint8_t> coverage;
    };
    mutable std::mutex mutex;
    mutable std::map<std::tuple<int, uint32_t, int>, Glyph> glyphs;

    static bool load_file(Font& font, const std::vector<fs::path>& files) {
#if !SFR_OVERLAY_TEXT
        (void)font;
        (void)files;
        return false;
#else
        for (const auto& path : files) {
            std::error_code error;
            if (!fs::is_regular_file(path, error)) continue;
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (data.size() < 12) continue;
            const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
            if (offset < 0) continue;
            font.data = std::move(data);
            if (stbtt_InitFont(&font.info, font.data.data(), offset)) {
                font.loaded = true;
                return true;
            }
            font.data.clear();
        }
        return false;
#endif
    }
    // The face that has the code point: the one asked for, the other Latin
    // face, then the CJK one (first for CJK code points, whose Latin faces'
    // fallback glyphs may be poor).
    int face_for(uint32_t c, int wanted) const {
#if !SFR_OVERLAY_TEXT
        (void)c;
        (void)wanted;
        return -1;
#else
        const int order_latin[3] = {wanted, wanted == 0 ? 1 : 0, 2};
        const int order_cjk[3] = {2, wanted, wanted == 0 ? 1 : 0};
        const int* order = is_cjk(c) ? order_cjk : order_latin;
        for (int i = 0; i < 3; ++i) {
            const Font& f = faces[size_t(order[i])];
            if (f.loaded && stbtt_FindGlyphIndex(&f.info, int(c))) return order[i];
        }
        for (int i = 0; i < 3; ++i)
            if (faces[size_t(order[i])].loaded) return order[i];
        return -1;
#endif
    }
    const Glyph* glyph(uint32_t c, float pixels, int wanted) const {
        const int face = face_for(c, wanted);
        if (face < 0) return nullptr;
        const int size = int(std::lround(pixels * 4.0f));  // quarter pixels
        const auto key = std::make_tuple(face, c, size);
        std::lock_guard lock(mutex);
        if (auto found = glyphs.find(key); found != glyphs.end()) return &found->second;
#if SFR_OVERLAY_TEXT
        const Font& f = faces[size_t(face)];
        const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, float(size) / 4.0f);
        Glyph g;
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&f.info, int(c), &advance, &bearing);
        g.advance = float(advance) * scale;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&f.info, int(c), scale, scale, &x0, &y0, &x1, &y1);
        g.width = (std::max)(0, x1 - x0);
        g.height = (std::max)(0, y1 - y0);
        g.x = x0;
        g.y = y0;
        if (g.width && g.height) {
            g.coverage.resize(size_t(g.width) * g.height);
            stbtt_MakeCodepointBitmap(&f.info, g.coverage.data(), g.width, g.height, g.width, scale, scale, int(c));
        }
        return &glyphs.emplace(key, std::move(g)).first->second;
#else
        return nullptr;
#endif
    }
};

OverlayFonts::OverlayFonts() : impl_(std::make_unique<Impl>()) {}
OverlayFonts::~OverlayFonts() = default;

void OverlayFonts::load(const std::vector<fs::path>& regular_files, const std::vector<fs::path>& bold_files,
                        const std::vector<fs::path>& cjk_files) {
    const OverlayFontCandidates host = overlay_font_candidates();
    Impl::load_file(impl_->faces[0], regular_files.empty() ? host.regular : regular_files);
    Impl::load_file(impl_->faces[1], bold_files.empty() ? host.bold : bold_files);
    Impl::load_file(impl_->faces[2], cjk_files.empty() ? host.cjk : cjk_files);
}

bool OverlayFonts::any() const {
    return std::any_of(impl_->faces.begin(), impl_->faces.end(), [](const Impl::Font& f) { return f.loaded; });
}

float OverlayFonts::measure(std::string_view text, float pixels, Face face) const {
    float width = 0;
    for (size_t at = 0; at < text.size();) {
        const uint32_t c = next_code_point(text, at);
        if (const auto* g = impl_->glyph(c, pixels, face)) width += g->advance;
    }
    return width;
}

OverlayCanvas::OverlayCanvas(uint32_t width, uint32_t height)
    : width_(width), height_(height), rgba_(size_t(width) * height * 4, 0) {}

void OverlayCanvas::blend(int x, int y, OverlayColor color, float coverage) {
    if (x < 0 || y < 0 || x >= int(width_) || y >= int(height_)) return;
    const float a = std::clamp(color.a * coverage, 0.0f, 1.0f);
    if (a <= 0.0f) return;
    uint8_t* p = rgba_.data() + (size_t(y) * width_ + size_t(x)) * 4;
    const float keep = 1.0f - a;
    p[0] = uint8_t(std::clamp(color.r * a * 255.0f + float(p[0]) * keep + 0.5f, 0.0f, 255.0f));
    p[1] = uint8_t(std::clamp(color.g * a * 255.0f + float(p[1]) * keep + 0.5f, 0.0f, 255.0f));
    p[2] = uint8_t(std::clamp(color.b * a * 255.0f + float(p[2]) * keep + 0.5f, 0.0f, 255.0f));
    p[3] = uint8_t(std::clamp(a * 255.0f + float(p[3]) * keep + 0.5f, 0.0f, 255.0f));
}

namespace {
// Signed distance from a pixel centre to a rounded rectangle (negative inside).
float round_rect_distance(float px, float py, float x, float y, float w, float h, float r) {
    r = (std::min)({r, w * 0.5f, h * 0.5f});
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float qx = std::fabs(px - cx) - (w * 0.5f - r), qy = std::fabs(py - cy) - (h * 0.5f - r);
    const float ox = (std::max)(qx, 0.0f), oy = (std::max)(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + (std::min)((std::max)(qx, qy), 0.0f) - r;
}
OverlayColor mix(OverlayColor a, OverlayColor b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
}

void OverlayCanvas::fill_round_rect(float x, float y, float w, float h, float radius, OverlayColor top, OverlayColor bottom) {
    if (w <= 0 || h <= 0) return;
    const int x0 = (std::max)(0, int(std::floor(x))), y0 = (std::max)(0, int(std::floor(y)));
    const int x1 = (std::min)(int(width_), int(std::ceil(x + w))), y1 = (std::min)(int(height_), int(std::ceil(y + h)));
    for (int py = y0; py < y1; ++py) {
        const OverlayColor color = mix(top, bottom, std::clamp((float(py) + 0.5f - y) / h, 0.0f, 1.0f));
        for (int px = x0; px < x1; ++px) {
            const float d = round_rect_distance(float(px) + 0.5f, float(py) + 0.5f, x, y, w, h, radius);
            blend(px, py, color, std::clamp(0.5f - d, 0.0f, 1.0f));
        }
    }
}

void OverlayCanvas::stroke_round_rect(float x, float y, float w, float h, float radius, float thickness, OverlayColor color) {
    if (w <= 0 || h <= 0) return;
    const int x0 = (std::max)(0, int(std::floor(x))), y0 = (std::max)(0, int(std::floor(y)));
    const int x1 = (std::min)(int(width_), int(std::ceil(x + w))), y1 = (std::min)(int(height_), int(std::ceil(y + h)));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            const float d = round_rect_distance(float(px) + 0.5f, float(py) + 0.5f, x, y, w, h, radius);
            const float outside = std::clamp(0.5f - d, 0.0f, 1.0f);
            const float inner = std::clamp(0.5f - (-d - thickness), 0.0f, 1.0f);
            blend(px, py, color, outside * inner);
        }
}

void OverlayCanvas::fill_circle(float cx, float cy, float radius, OverlayColor color) {
    fill_round_rect(cx - radius, cy - radius, radius * 2, radius * 2, radius, color);
}

void OverlayCanvas::draw_image(const DecodedImage& image, float x, float y, float w, float h, float radius, float opacity,
                               bool grey) {
    if (!image || w <= 0 || h <= 0) return;
    const int x0 = (std::max)(0, int(std::floor(x))), y0 = (std::max)(0, int(std::floor(y)));
    const int x1 = (std::min)(int(width_), int(std::ceil(x + w))), y1 = (std::min)(int(height_), int(std::ceil(y + h)));
    const auto texel = [&](int tx, int ty, int c) {
        tx = std::clamp(tx, 0, int(image.width) - 1);
        ty = std::clamp(ty, 0, int(image.height) - 1);
        return float(image.rgba[(size_t(ty) * image.width + size_t(tx)) * 4 + size_t(c)]);
    };
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            const float d = round_rect_distance(float(px) + 0.5f, float(py) + 0.5f, x, y, w, h, radius);
            const float edge = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (edge <= 0) continue;
            // Area average when shrinking a lot, bilinear otherwise.
            const float u = (float(px) + 0.5f - x) / w * float(image.width) - 0.5f;
            const float v = (float(py) + 0.5f - y) / h * float(image.height) - 0.5f;
            const int ix = int(std::floor(u)), iy = int(std::floor(v));
            const float fx = u - float(ix), fy = v - float(iy);
            float value[4];
            for (int c = 0; c < 4; ++c) {
                const float top = texel(ix, iy, c) * (1 - fx) + texel(ix + 1, iy, c) * fx;
                const float bottom = texel(ix, iy + 1, c) * (1 - fx) + texel(ix + 1, iy + 1, c) * fx;
                value[c] = (top * (1 - fy) + bottom * fy) / 255.0f;
            }
            OverlayColor color{value[0], value[1], value[2], value[3] * opacity};
            if (grey) {
                const float l = (0.3f * color.r + 0.59f * color.g + 0.11f * color.b) * 0.55f;
                color.r = color.g = color.b = l;
            }
            blend(px, py, color, edge);
        }
}

float OverlayCanvas::text(const OverlayFonts& fonts, std::string_view text, float x, float baseline, float pixels,
                          OverlayColor color, OverlayFonts::Face face, float max_width) {
    std::string shown(text);
    if (max_width > 0 && fonts.measure(shown, pixels, face) > max_width) {
        const float dots = fonts.measure("...", pixels, face);
        std::string cut;
        float width = 0;
        for (size_t at = 0; at < text.size();) {
            const size_t start = at;
            const uint32_t c = next_code_point(text, at);
            const float w = fonts.measure(text.substr(start, at - start), pixels, face);
            if (width + w + dots > max_width) break;
            width += w;
            (void)c;
            cut.append(text.substr(start, at - start));
        }
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        shown = cut + "...";
    }
    float pen = x;
    for (size_t at = 0; at < shown.size();) {
        const uint32_t c = next_code_point(shown, at);
        const auto* g = fonts.impl().glyph(c, pixels, face);
        if (!g) break;
        const int gx = int(std::lround(pen)) + g->x, gy = int(std::lround(baseline)) + g->y;
        for (int row = 0; row < g->height; ++row)
            for (int column = 0; column < g->width; ++column)
                if (const uint8_t cover = g->coverage[size_t(row) * g->width + size_t(column)])
                    blend(gx + column, gy + row, color, float(cover) / 255.0f);
        pen += g->advance;
    }
    return pen - x;
}

std::vector<std::string> wrap_overlay_text(const OverlayFonts& fonts, std::string_view text, float pixels,
                                           OverlayFonts::Face face, float max_width, size_t max_lines) {
    std::vector<std::string> lines;
    if (!max_lines) return lines;
    std::string line;
    size_t at = 0;
    // Words: runs up to a space, or single CJK characters.
    while (at < text.size()) {
        size_t end = at;
        const uint32_t first = next_code_point(text, end);
        if (!is_cjk(first) && first != ' ') {
            size_t probe = end;
            while (probe < text.size()) {
                size_t next = probe;
                const uint32_t c = next_code_point(text, next);
                if (c == ' ' || is_cjk(c)) break;
                probe = next;
            }
            end = probe;
        }
        const std::string word(text.substr(at, end - at));
        at = end;
        if (word == " " && line.empty()) continue;
        const std::string candidate = line + word;
        if (!line.empty() && fonts.measure(candidate, pixels, face) > max_width) {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            lines.push_back(line);
            line = word == " " ? std::string() : word;
            if (lines.size() == max_lines) {
                // Text is left over: the last line ends in "...".
                std::string& last = lines.back();
                std::string rest = last + " " + line + std::string(text.substr(at));
                while (!last.empty() && fonts.measure(last + "...", pixels, face) > max_width) {
                    size_t cut = last.size() - 1;
                    while (cut > 0 && (uint8_t(last[cut]) & 0xC0) == 0x80) --cut;
                    last.resize(cut);
                }
                (void)rest;
                last += "...";
                return lines;
            }
        } else {
            line = candidate;
        }
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    if (!line.empty()) lines.push_back(line);
    return lines;
}
}
