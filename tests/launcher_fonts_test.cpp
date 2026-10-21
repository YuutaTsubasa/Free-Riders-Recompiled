#include "launcher_fonts.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using sfr::launcher::atlas_fits;
using sfr::launcher::font_face_loadable;
using Bytes = std::vector<unsigned char>;

void put32(Bytes& bytes, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(uint8_t(value >> shift));
}
void put16(Bytes& bytes, uint16_t value) {
    bytes.push_back(uint8_t(value >> 8));
    bytes.push_back(uint8_t(value));
}
uint32_t tag(const char* name) {
    return uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 | uint32_t(uint8_t(name[2])) << 8 |
           uint8_t(name[3]);
}

// An sfnt table directory (the tables' contents do not matter here).
Bytes face(uint32_t version, std::initializer_list<const char*> tables) {
    Bytes bytes;
    put32(bytes, version);
    put16(bytes, uint16_t(tables.size()));
    put16(bytes, 0);
    put16(bytes, 0);
    put16(bytes, 0);
    for (const char* name : tables) {
        put32(bytes, tag(name));
        put32(bytes, 0);
        put32(bytes, 0);
        put32(bytes, 0);
    }
    return bytes;
}

// A font collection whose first face is `first`.
Bytes collection(const Bytes& first, uint32_t count = 2) {
    Bytes bytes;
    put32(bytes, tag("ttcf"));
    put32(bytes, 0x00010000);
    put32(bytes, count);
    const uint32_t offset = 12 + 4 * count;
    for (uint32_t i = 0; i < count; ++i) put32(bytes, offset);
    bytes.insert(bytes.end(), first.begin(), first.end());
    return bytes;
}

bool loadable(const Bytes& bytes) { return font_face_loadable(bytes.data(), bytes.size()); }

constexpr uint32_t truetype = 0x00010000, otto = 0x4F54544F;

void faces() {
    require(loadable(face(truetype, {"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name"})),
            "a TrueType face loads");
    require(loadable(face(otto, {"CFF ", "cmap", "head", "hhea", "hmtx", "maxp"})), "a CFF face loads");
    require(loadable(face(truetype, {"cmap", "fvar", "glyf", "gvar", "head", "hhea", "hmtx", "loca"})),
            "a TrueType variable face loads (its default outlines)");
    require(!loadable(face(otto, {"CFF2", "cmap", "fvar", "head", "hhea", "hmtx", "maxp"})),
            "a CFF2 face (Fedora's NotoSansCJK-VF) does not");
    require(!loadable(face(truetype, {"cmap", "glyf", "head", "hhea", "hmtx"})), "glyf without loca does not");
    require(!loadable(face(truetype, {"glyf", "head", "hhea", "hmtx", "loca"})), "no cmap does not");
    require(!loadable(face(truetype, {"cmap", "glyf", "hhea", "hmtx", "loca"})), "no head does not");
    require(!loadable(face(0x774F4646 /* wOFF */, {"cmap", "glyf", "head", "hhea", "hmtx", "loca"})),
            "a WOFF file does not");
}

void collections() {
    const Bytes cff = face(otto, {"CFF ", "cmap", "head", "hhea", "hmtx"});
    const Bytes cff2 = face(otto, {"CFF2", "cmap", "head", "hhea", "hmtx"});
    require(loadable(collection(cff)), "a collection of CFF faces (NotoSansCJK-Regular.ttc) loads");
    require(!loadable(collection(cff2)), "a collection of CFF2 faces (NotoSansCJK-VF.ttc) does not");
    require(!loadable(collection(cff, 0)), "an empty collection does not");
    Bytes cut = collection(cff);
    cut.resize(cut.size() - 8);
    require(!loadable(cut), "a table directory cut short does not");
    Bytes far = collection(cff);
    far[12] = 0x7F;  // the first face's offset beyond the file
    require(!loadable(far), "a face beyond the file does not");
    require(!font_face_loadable(nullptr, 0), "nothing does not");
    require(!loadable(Bytes(11, 0)), "a short file does not");
}

void files() {
    const auto path = std::filesystem::temp_directory_path() / "sfr_launcher_fonts_test.ttc";
    const Bytes bytes = collection(face(otto, {"CFF2", "cmap", "head", "hhea", "hmtx"}));
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    require(!sfr::launcher::font_file_loadable(path), "a CFF2 collection on disk does not load");
    std::filesystem::remove(path);
    require(!sfr::launcher::font_file_loadable(path), "a missing file does not load");
}

void atlas() {
    require(atlas_fits(1024, 4096, 16384, 16384), "a 1024 x 4096 atlas fits a 16384 texture");
    require(atlas_fits(1024, 4096, 0, 0), "anything built fits a renderer without a limit (software)");
    require(!atlas_fits(1024, 4096, 2048, 2048), "too tall for a 2048 texture");
    require(!atlas_fits(4096, 1024, 2048, 2048), "too wide for a 2048 texture");
    require(!atlas_fits(0, 0, 16384, 16384), "an atlas that did not build (0 x 0) does not fit");
    require(!atlas_fits(0, 0, 0, 0), "nor without a limit");
}
}  // namespace

int main() {
    try {
        faces();
        collections();
        files();
        atlas();
    } catch (const std::exception& error) {
        std::cerr << "launcher_fonts_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "launcher_fonts_test: ok\n";
    return 0;
}
