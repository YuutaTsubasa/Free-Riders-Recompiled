#include "installer.h"
#include "xex_image.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
bool throws(Operation operation) {
    try { operation(); } catch (const std::runtime_error&) { return true; }
    return false;
}

// A small XDVDFS image built by hand: descriptor at sector 32, directory
// tables and files after it.
struct Builder {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(64 * 2048);
    void put16(size_t at, uint16_t v) { bytes[at] = uint8_t(v); bytes[at + 1] = uint8_t(v >> 8); }
    void put32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[at + i] = uint8_t(v >> (8 * i)); }
    void descriptor(uint32_t root_sector, uint32_t root_size) {
        const size_t at = 32 * 2048;
        std::memcpy(&bytes[at], "MICROSOFT*XBOX*MEDIA", 20);
        put32(at + 20, root_sector);
        put32(at + 24, root_size);
        std::memcpy(&bytes[at + 2048 - 20], "MICROSOFT*XBOX*MEDIA", 20);
    }
    // One node at table byte offset; left/right are byte offsets (multiples of 4).
    void node(uint32_t table_sector, uint32_t offset, uint32_t left, uint32_t right, uint32_t sector, uint32_t size,
              bool directory, const std::string& name) {
        const size_t at = table_sector * 2048 + offset;
        put16(at, uint16_t(left / 4));
        put16(at + 2, uint16_t(right / 4));
        put32(at + 4, sector);
        put32(at + 8, size);
        bytes[at + 12] = directory ? 0x10 : 0x20;
        bytes[at + 13] = uint8_t(name.size());
        std::memcpy(&bytes[at + 14], name.data(), name.size());
    }
    void file(uint32_t sector, const std::string& text) { std::memcpy(&bytes[sector * 2048], text.data(), text.size()); }
    fs::path write(const fs::path& path) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return path;
    }
};

// Root (sector 33): "b.txt" with left child "a" (a directory at sector 34
// holding "c.bin").
Builder sample() {
    Builder b;
    b.descriptor(33, 2048);
    b.node(33, 0, 20, 0, 40, 5, false, "b.txt");
    b.node(33, 20, 0, 0, 34, 2048, true, "a");
    b.node(34, 0, 0, 0, 41, 3, false, "c.bin");
    b.file(40, "hello");
    b.file(41, "xyz");
    return b;
}

void lists_and_reads_a_disc(const fs::path& root) {
    sfr::DiscImage disc(sample().write(root / "sample.iso"));
    std::set<std::string> paths;
    for (const auto& entry : disc.entries()) paths.insert(entry.path);
    require(paths == std::set<std::string>{"b.txt", "a", "a/c.bin"}, "every file and directory is listed");
    const auto* c = disc.find("A/C.BIN");
    require(c && !c->directory && c->size == 3, "lookup ignores case");
    const auto bytes = disc.read_file(*c);
    require(std::string(bytes.begin(), bytes.end()) == "xyz", "file contents are read from its extent");
    std::vector<uint8_t> part(2);
    disc.read(*disc.find("b.txt"), 3, part);
    require(part[0] == 'l' && part[1] == 'o', "a file is read from an offset");
    require(throws([&] { disc.read(*c, 2, part); }), "reads past a file's end are refused");
}

void rejects_malformed_discs(const fs::path& root) {
    Builder no_marker;
    require(throws([&] { sfr::DiscImage d(no_marker.write(root / "empty.iso")); }), "an image without XDVDFS is refused");

    Builder cycle = sample();
    cycle.node(33, 20, 0, 0, 33, 2048, true, "a");  // "a" is the root table again
    require(throws([&] { sfr::DiscImage d(cycle.write(root / "cycle.iso")); }), "a directory cycle is refused");

    Builder outside = sample();
    outside.node(34, 0, 0, 0, 5000, 3, false, "c.bin");
    require(throws([&] { sfr::DiscImage d(outside.write(root / "outside.iso")); }), "an extent past the end is refused");

    Builder reserved = sample();
    reserved.node(34, 0, 0, 0, 41, 3, false, "CON.x");
    require(throws([&] { sfr::DiscImage d(reserved.write(root / "reserved.iso")); }), "reserved names are refused");

    Builder traversal = sample();
    traversal.node(34, 0, 0, 0, 41, 3, false, "..");
    require(throws([&] { sfr::DiscImage d(traversal.write(root / "dots.iso")); }), "parent references are refused");

    Builder duplicate = sample();
    duplicate.node(33, 20, 0, 0, 41, 3, false, "B.TXT");
    require(throws([&] { sfr::DiscImage d(duplicate.write(root / "duplicate.iso")); }), "case-colliding names are refused");

    Builder shared = sample();
    shared.node(33, 0, 20, 20, 40, 5, false, "b.txt");  // both children are "a"
    require(throws([&] { sfr::DiscImage d(shared.write(root / "shared.iso")); }), "a node reached twice is refused");
}

void inspects_sources(const fs::path& root) {
    auto summary = sfr::inspect_install_source(root / "sample.iso");
    require(summary.kind == sfr::SourceKind::disc_image && summary.problem == sfr::SourceProblem::not_a_disc,
            "a disc without default.xex is not installable");
    require(summary.file_count == 2 && summary.total_bytes == 8, "the disc's files are counted");

    Builder impostor = sample();
    impostor.node(33, 0, 20, 40, 40, 5, false, "b.txt");
    impostor.node(33, 40, 0, 0, 42, 4, false, "default.xex");
    impostor.file(42, "XEX2");
    summary = sfr::inspect_install_source(impostor.write(root / "impostor.iso"));
    require(summary.problem == sfr::SourceProblem::wrong_game, "another default.xex is the wrong game");

    const fs::path folder = root / "folder";
    fs::create_directories(folder / "sub");
    std::ofstream(folder / "sub" / "data.bin") << "12345";
    summary = sfr::inspect_install_source(folder);
    require(summary.kind == sfr::SourceKind::folder && summary.problem == sfr::SourceProblem::not_a_disc,
            "a folder without default.xex is not installable");
    require(summary.file_count == 1 && summary.total_bytes == 5, "a folder's files are counted");
    std::ofstream(folder / "default.xex") << "XEX2";
    summary = sfr::inspect_install_source(folder);
    require(summary.problem == sfr::SourceProblem::wrong_game, "a folder with another default.xex is the wrong game");

    summary = sfr::inspect_install_source(root / "missing.iso");
    require(summary.kind == sfr::SourceKind::none && summary.problem == sfr::SourceProblem::unreadable,
            "a missing source is unreadable");

    const fs::path game = root / "game";
    require(throws([&] { sfr::install_game(folder, game, [](auto...) { return true; }); }),
            "the wrong game is not installed");
    require(!fs::exists(game / ".install-partial") && !fs::exists(game / "assets"), "a refused install leaves nothing");
    bool cancelled = false;
    try {
        sfr::install_game(folder, game, [](auto...) { return false; });
    } catch (const sfr::InstallCancelled&) {
        cancelled = true;
    }
    require(cancelled && !fs::exists(game / "assets"), "cancelling stops before anything is written");
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(x), {}) == std::vector<char>(std::istreambuf_iterator<char>(y), {});
}

// With SFR_SOURCE_ISO and SFR_IMAGE_DIRECTORY set: install the real disc
// and compare the decoded image with the reference dump.
void installs_the_real_disc(const fs::path& root) {
    const char* iso = std::getenv("SFR_SOURCE_ISO");
    const char* reference = std::getenv("SFR_IMAGE_DIRECTORY");
    if (!iso || !reference) {
        std::cout << "SKIP real disc install (set SFR_SOURCE_ISO and SFR_IMAGE_DIRECTORY)\n";
        return;
    }
    const auto summary = sfr::inspect_install_source(iso);
    require(summary.problem == sfr::SourceProblem::none, "the real disc is supported");
    const fs::path game = root / "real";
    uint64_t last = 0;
    bool decoded = false;
    sfr::install_game(iso, game, [&](sfr::InstallStage stage, uint64_t done, uint64_t total, const std::string&) {
        if (stage == sfr::InstallStage::copying) {
            require(done >= last && done <= total && total == summary.total_bytes, "copy progress only moves forward");
            last = done;
        }
        if (stage == sfr::InstallStage::decoding) decoded = true;
        return true;
    });
    require(decoded && last == summary.total_bytes, "every byte was copied, then the image decoded");
    for (const char* name : {"image.bin", "image.tsv", "imports.tsv", "import_variables.tsv", "sections.tsv", "xex_header.bin"})
        require(same_file(game / "image" / name, fs::path(reference) / name), "the installed image matches the reference dump");
    require(fs::is_regular_file(game / "image" / "complete.txt") && fs::is_regular_file(game / "assets" / "default.xex"),
            "the installation is complete");
    require(!fs::exists(game / ".install-partial"), "the staging folder is gone");
    std::cout << "Installed " << summary.file_count << " files from the real disc\n";
}
}

int main() {
    const fs::path root = fs::temp_directory_path() / "sfr_installer_test";
    try {
        fs::remove_all(root);
        fs::create_directories(root);
        lists_and_reads_a_disc(root);
        rejects_malformed_discs(root);
        inspects_sources(root);
        installs_the_real_disc(root);
        fs::remove_all(root);
        std::cout << "Installer checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
