#pragma once

// Which font files the launcher's ImGui can rasterize, and whether its font
// atlas fits the renderer. Issue #232: Fedora's only Chinese font is
// NotoSansCJK-VF.ttc, whose outlines are CFF2; ImGui's stb_truetype reads
// TrueType ('glyf') and CFF outlines only, so the whole atlas failed to build
// (0 x 0), the font texture was never made, and ImGui::NewFrame crashed.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace sfr::launcher {

namespace font_detail {
inline uint32_t be32(const unsigned char* at) {
    return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | uint32_t(at[3]);
}
inline uint16_t be16(const unsigned char* at) { return uint16_t(at[0] << 8 | at[1]); }
}  // namespace font_detail

// True when face 0 of `data` (a font file's beginning; a collection's
// first face is what ImGui loads) has the tables stb_truetype's
// stbtt_InitFont needs: cmap, head, hhea, hmtx, and either glyf with loca
// or CFF. A table directory past the end of `data` counts as unusable.
inline bool font_face_loadable(const unsigned char* data, size_t size) {
    using font_detail::be16;
    using font_detail::be32;
    if (!data || size < 12) return false;
    size_t face = 0;
    if (be32(data) == 0x74746366u) {  // 'ttcf'
        if (size < 16 || be32(data + 8) == 0) return false;
        face = be32(data + 12);
        if (face > size || size - face < 12) return false;
    }
    const uint32_t version = be32(data + face);
    if (version != 0x00010000u && version != 0x4F54544Fu /* OTTO */ && version != 0x74727565u /* true */ &&
        version != 0x31000000u /* '1\0\0\0' */)
        return false;
    const size_t tables = be16(data + face + 4);
    if ((size - face - 12) / 16 < tables) return false;
    bool cmap = false, head = false, hhea = false, hmtx = false, glyf = false, loca = false, cff = false;
    for (size_t i = 0; i < tables; ++i) {
        const unsigned char* tag = data + face + 12 + 16 * i;
        switch (be32(tag)) {
        case 0x636D6170u: cmap = true; break;  // cmap
        case 0x68656164u: head = true; break;  // head
        case 0x68686561u: hhea = true; break;  // hhea
        case 0x686D7478u: hmtx = true; break;  // hmtx
        case 0x676C7966u: glyf = true; break;  // glyf
        case 0x6C6F6361u: loca = true; break;  // loca
        case 0x43464620u: cff = true; break;   // 'CFF ' (not CFF2)
        default: break;
        }
    }
    return cmap && head && hhea && hmtx && ((glyf && loca) || cff);
}

// The same for a file: its table directories are at the start.
inline bool font_file_loadable(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<unsigned char> head(64 * 1024);
    in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
    return font_face_loadable(head.data(), size_t(in.gcount()));
}

// A renderer's texture limit: 0 means none (SDL's software renderer).
inline bool atlas_fits(int width, int height, int max_width, int max_height) {
    if (width <= 0 || height <= 0) return false;  // the atlas did not build
    return (max_width <= 0 || width <= max_width) && (max_height <= 0 || height <= max_height);
}

// What the launcher's fonts are built from, best first: each later set is
// tried when the one before does not build or fit the renderer.
enum class FontSet {
    system = 0,         // the system's Latin fonts with Chinese merged in
    system_latin = 1,   // the same without Chinese (the launcher shows English)
    builtin = 2,        // ImGui's own small font, scaled
};
constexpr int font_set_count = 3;

inline const char* font_set_name(FontSet set) {
    switch (set) {
    case FontSet::system: return "system";
    case FontSet::system_latin: return "system-latin";
    case FontSet::builtin: return "builtin";
    }
    return "?";
}

}  // namespace sfr::launcher
