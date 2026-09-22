#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {
// Read-only XDVDFS (Xbox 360 game disc) reader with the bounds of
// scripts/freeriders/disc.py: every extent inside the file, directory
// tables without overlap or cycles, safe path components, no duplicate
// (case-insensitive) paths, and budgets on directories and entries.
struct DiscEntry {
    std::string path;  // '/'-separated, as on the disc
    uint32_t sector = 0, size = 0;
    bool directory = false;
};

class DiscImage {
public:
    // Throws std::runtime_error when the file is not a readable XDVDFS image.
    explicit DiscImage(const std::filesystem::path& file);
    ~DiscImage();
    DiscImage(const DiscImage&) = delete;
    DiscImage& operator=(const DiscImage&) = delete;
    const std::vector<DiscEntry>& entries() const { return entries_; }
    const DiscEntry* find(std::string_view path) const;
    uint64_t partition_offset() const { return partition_; }
    // Reads part of a file.
    void read(const DiscEntry& entry, uint64_t offset, std::span<uint8_t> out);
    std::vector<uint8_t> read_file(const DiscEntry& entry);
private:
    void read_at(uint64_t offset, std::span<uint8_t> out);
    uint64_t extent(uint32_t sector, uint32_t size) const;
#ifdef _WIN32
    std::ifstream file_;
#else
    // POSIX: read with pread. "/proc/self/fd/N" (a document Android's picker
    // opened for the app) is read through that descriptor, never reopened:
    // a fresh open goes back to the storage provider, which refuses it.
    int descriptor_ = -1;
#endif
    uint64_t size_ = 0, partition_ = 0;
    std::vector<DiscEntry> entries_;
};

// What the player picked to install from.
enum class SourceKind { none, disc_image, folder };
enum class SourceProblem {
    none,
    unreadable,     // missing, or cannot be opened
    not_a_disc,     // not an XDVDFS image / no default.xex in the folder
    wrong_game,     // default.xex is not the supported Sonic Free Riders
};
struct SourceSummary {
    SourceKind kind = SourceKind::none;
    SourceProblem problem = SourceProblem::unreadable;
    uint64_t total_bytes = 0;  // of the files to copy
    size_t file_count = 0;
    std::string detail;        // English, for the log
};
// A .iso (or any file) is read as a disc image, a directory as the disc's
// extracted files.
SourceSummary inspect_install_source(const std::filesystem::path& source);

enum class InstallStage { checking, copying, decoding, finishing };
// Called often from the installing thread; returning false cancels.
using InstallProgress = std::function<bool(InstallStage stage, uint64_t done, uint64_t total, const std::string& item)>;

struct InstallCancelled : std::runtime_error {
    InstallCancelled() : std::runtime_error("installation cancelled") {}
};

// Installs into game_directory/assets (the disc's files) and
// game_directory/image (the decoded code image), staged in
// game_directory/.install-partial and moved into place only when complete;
// a previous installation is replaced then. Throws std::runtime_error (or
// InstallCancelled) and leaves no partial files behind.
void install_game(const std::filesystem::path& source, const std::filesystem::path& game_directory,
                  const InstallProgress& progress);

// Space the installation needs on the destination drive: the files plus
// the decoded image and some margin.
uint64_t install_space_needed(const SourceSummary& summary);
}
