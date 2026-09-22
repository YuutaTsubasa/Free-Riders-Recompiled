#include "installer.h"
#include "xex_image.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sfr {
namespace fs = std::filesystem;
namespace {
constexpr uint64_t sector_size = 2048;
constexpr std::array<uint64_t, 3> partition_bases{0, 0xFD90000, 0x2080000};
constexpr char marker[] = "MICROSOFT*XBOX*MEDIA";
constexpr uint32_t max_directory_bytes = 64u << 20, max_total_directory_bytes = 64u << 20;
constexpr size_t max_directories = 4096, max_entries = 1'000'000, max_depth = 256;

uint16_t le16(const uint8_t* at) { return uint16_t(at[0] | at[1] << 8); }
uint32_t le32(const uint8_t* at) { return uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24; }

std::string folded(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void validate_name(const std::string& name) {
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (auto& c : stem) c = char(std::toupper(static_cast<unsigned char>(c)));
    static const std::set<std::string> reserved = [] {
        std::set<std::string> names{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
        for (const char* prefix : {"COM", "LPT"})
            for (char digit = '1'; digit <= '9'; ++digit) names.insert(std::string(prefix) + digit);
        return names;
    }();
    bool bad = name.empty() || name == "." || name == ".." || name.back() == ' ' || name.back() == '.' ||
               reserved.count(stem) != 0;
    for (const unsigned char c : name)
        if (c < 32 || std::strchr("/\\:*?\"<>|", c)) bad = true;
    if (bad) throw std::runtime_error("unsafe disc path component: " + name);
}
}

DiscImage::DiscImage(const fs::path& path)
#ifdef _WIN32
    : file_(path, std::ios::binary) {
    if (!file_) throw std::runtime_error("cannot open the disc image");
    std::error_code error;
    size_ = fs::file_size(path, error);
    if (error) throw std::runtime_error("cannot size the disc image");
#else
{
    const std::string text = path.string();
    constexpr std::string_view passed = "/proc/self/fd/";
    if (text.rfind(passed, 0) == 0 && text.size() > passed.size() &&
        std::all_of(text.begin() + std::ptrdiff_t(passed.size()), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        descriptor_ = dup(std::stoi(text.substr(passed.size())));
    else
        descriptor_ = open(text.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor_ < 0) throw std::runtime_error("cannot open the disc image (error " + std::to_string(errno) + ")");
    struct stat info {};
    if (fstat(descriptor_, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error("the disc image is not a regular file (a cloud document? copy it to the device first)");
    }
    size_ = uint64_t(info.st_size);
#endif

    std::vector<uint8_t> descriptor(sector_size);
    bool found = false;
    for (const uint64_t base : partition_bases) {
        const uint64_t offset = base + 32 * sector_size;
        if (offset + sector_size > size_) continue;
        read_at(offset, descriptor);
        if (std::memcmp(descriptor.data(), marker, 20) != 0) continue;
        if (std::memcmp(descriptor.data() + sector_size - 20, marker, 20) != 0)
            throw std::runtime_error("XDVDFS descriptor closing marker is invalid");
        partition_ = base;
        found = true;
        break;
    }
    if (!found) throw std::runtime_error("no XDVDFS volume descriptor found");

    struct Pending { std::string parent; uint32_t sector, size; size_t depth; };
    std::vector<Pending> directories;
    std::map<uint64_t, uint64_t> directory_extents;  // start -> end, never overlapping
    uint64_t total_directory_bytes = 0;
    size_t directory_count = 0;
    // Every extent and its budget is reserved before it is read.
    const auto enqueue = [&](std::string parent, uint32_t sector, uint32_t size, size_t depth) {
        const uint64_t start = extent(sector, size);
        if (size > max_directory_bytes || depth > max_depth || ++directory_count > max_directories ||
            size > max_total_directory_bytes - total_directory_bytes)
            throw std::runtime_error("XDVDFS directory resource limit exceeded");
        if (!size) return;
        const uint64_t end = start + size;
        const auto next = directory_extents.lower_bound(start);
        if ((next != directory_extents.end() && next->first < end) ||
            (next != directory_extents.begin() && std::prev(next)->second > start))
            throw std::runtime_error("cyclic, shared or overlapping XDVDFS directory extent");
        directory_extents.emplace(start, end);
        total_directory_bytes += size;
        directories.push_back({std::move(parent), sector, size, depth});
    };
    enqueue("", le32(descriptor.data() + 20), le32(descriptor.data() + 24), 0);

    std::set<std::string> paths;
    while (!directories.empty()) {
        const Pending directory = std::move(directories.back());
        directories.pop_back();
        std::vector<uint8_t> table(directory.size);
        read_at(extent(directory.sector, directory.size), table);
        std::vector<uint8_t> occupied(directory.size);
        std::vector<uint32_t> nodes{0};
        while (!nodes.empty()) {
            const uint32_t offset = nodes.back();
            nodes.pop_back();
            if (uint64_t(offset) + 14 > directory.size) throw std::runtime_error("XDVDFS node is out of bounds");
            const uint8_t* node = table.data() + offset;
            const uint16_t left = le16(node), right = le16(node + 2);
            const uint32_t sector = le32(node + 4), size = le32(node + 8);
            const uint8_t attributes = node[12], name_size = node[13];
            const uint64_t end = uint64_t(offset) + 14 + name_size;
            if (end > directory.size ||
                std::any_of(occupied.begin() + offset, occupied.begin() + std::ptrdiff_t(end), [](uint8_t b) { return b != 0; }))
                throw std::runtime_error("XDVDFS node is truncated, overlapping or cyclic");
            std::fill(occupied.begin() + offset, occupied.begin() + std::ptrdiff_t(end), uint8_t(1));
            const std::string name(reinterpret_cast<const char*>(node + 14), name_size);
            validate_name(name);
            std::string path = directory.parent.empty() ? name : directory.parent + "/" + name;
            if (!paths.insert(folded(path)).second)
                throw std::runtime_error("duplicate or case-colliding XDVDFS path: " + path);
            const bool is_directory = (attributes & 0x10) != 0;
            extent(sector, size);
            entries_.push_back({path, sector, size, is_directory});
            if (entries_.size() > max_entries) throw std::runtime_error("XDVDFS entry count resource limit exceeded");
            if (is_directory) enqueue(path, sector, size, directory.depth + 1);
            if (right) nodes.push_back(uint32_t(right) * 4);
            if (left) nodes.push_back(uint32_t(left) * 4);
        }
    }
}

uint64_t DiscImage::extent(uint32_t sector, uint32_t size) const {
    const uint64_t offset = partition_ + uint64_t(sector) * sector_size;
    if (offset > size_ || size > size_ - offset) throw std::runtime_error("XDVDFS extent is out of bounds");
    return offset;
}

DiscImage::~DiscImage() {
#ifndef _WIN32
    if (descriptor_ >= 0) close(descriptor_);
#endif
}

void DiscImage::read_at(uint64_t offset, std::span<uint8_t> out) {
    if (offset > size_ || out.size() > size_ - offset) throw std::runtime_error("disc read is out of bounds");
#ifdef _WIN32
    file_.clear();
    file_.seekg(std::streamoff(offset));
    if (!file_.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())))
        throw std::runtime_error("disc read was truncated");
#else
    for (size_t done = 0; done < out.size();) {
        const ssize_t got = pread(descriptor_, out.data() + done, out.size() - done, off_t(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) throw std::runtime_error("disc read failed (error " + std::to_string(errno) + ")");
        if (got == 0) throw std::runtime_error("disc read was truncated");
        done += size_t(got);
    }
#endif
}

const DiscEntry* DiscImage::find(std::string_view path) const {
    const std::string wanted = folded(path);
    for (const auto& entry : entries_)
        if (folded(entry.path) == wanted) return &entry;
    return nullptr;
}

void DiscImage::read(const DiscEntry& entry, uint64_t offset, std::span<uint8_t> out) {
    if (entry.directory || offset > entry.size || out.size() > entry.size - offset)
        throw std::runtime_error("disc file read is out of bounds");
    read_at(extent(entry.sector, entry.size) + offset, out);
}

std::vector<uint8_t> DiscImage::read_file(const DiscEntry& entry) {
    std::vector<uint8_t> bytes(entry.size);
    read(entry, 0, bytes);
    return bytes;
}

namespace {
// A folder's regular files, relative, '/'-separated; links and junctions
// are not followed.
std::vector<std::pair<std::string, uint64_t>> folder_files(const fs::path& root) {
    std::vector<std::pair<std::string, uint64_t>> files;
    std::error_code error;
    fs::recursive_directory_iterator walk(root, fs::directory_options::none, error), end;
    if (error) throw std::runtime_error("cannot list the folder");
    for (; walk != end; walk.increment(error)) {
        if (error) throw std::runtime_error("cannot list the folder");
        const auto status = walk->symlink_status(error);
        if (status.type() == fs::file_type::directory) continue;
        if (status.type() != fs::file_type::regular) {
            walk.disable_recursion_pending();
            continue;
        }
        const auto relative = fs::relative(walk->path(), root, error).generic_u8string();
        files.emplace_back(std::string(relative.begin(), relative.end()), walk->file_size(error));
        if (files.size() > max_entries) throw std::runtime_error("folder has too many files");
    }
    return files;
}

std::vector<uint8_t> read_whole(const fs::path& path, uint64_t limit) {
    std::error_code error;
    const uint64_t size = fs::file_size(path, error);
    if (error || size > limit) return {};
    std::vector<uint8_t> bytes(size);
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) return {};
    return bytes;
}

fs::path utf8_path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
}

SourceSummary inspect_install_source(const fs::path& source) {
    SourceSummary summary;
    std::error_code error;
    if (fs::is_directory(source, error)) {
        summary.kind = SourceKind::folder;
        try {
            const auto files = folder_files(source);
            summary.file_count = files.size();
            for (const auto& [path, size] : files) summary.total_bytes += size;
        } catch (const std::exception& e) {
            summary.detail = e.what();
            return summary;
        }
        if (!fs::is_regular_file(source / "default.xex", error)) {
            summary.problem = SourceProblem::not_a_disc;
            summary.detail = "the folder has no default.xex";
            return summary;
        }
        const auto xex = read_whole(source / "default.xex", supported_xex_size);
        summary.problem = is_supported_xex(xex) ? SourceProblem::none : SourceProblem::wrong_game;
        if (summary.problem != SourceProblem::none) summary.detail = "default.xex is not the supported game";
        return summary;
    }
    if (!fs::is_regular_file(source, error)) {
        summary.detail = "not found";
        return summary;
    }
    summary.kind = SourceKind::disc_image;
    try {
        DiscImage disc(source);
        for (const auto& entry : disc.entries())
            if (!entry.directory) { ++summary.file_count; summary.total_bytes += entry.size; }
        const DiscEntry* xex = disc.find("default.xex");
        if (!xex || xex->directory) {
            summary.problem = SourceProblem::not_a_disc;
            summary.detail = "the disc has no default.xex";
            return summary;
        }
        const bool supported = xex->size == supported_xex_size && is_supported_xex(disc.read_file(*xex));
        summary.problem = supported ? SourceProblem::none : SourceProblem::wrong_game;
        if (!supported) summary.detail = "default.xex is not the supported game";
    } catch (const std::exception& e) {
        summary.problem = SourceProblem::not_a_disc;
        summary.detail = e.what();
    }
    return summary;
}

uint64_t install_space_needed(const SourceSummary& summary) {
    return summary.total_bytes + (48ull << 20) + (64ull << 20);  // image.bin is about 33 MiB
}

void install_game(const fs::path& source, const fs::path& game_directory, const InstallProgress& progress) {
    if (!progress(InstallStage::checking, 0, 0, {})) throw InstallCancelled();
    const SourceSummary summary = inspect_install_source(source);
    if (summary.problem != SourceProblem::none)
        throw std::runtime_error("the source cannot be installed: " + summary.detail);

    const fs::path stage = game_directory / ".install-partial";
    std::error_code error;
    fs::remove_all(stage, error);  // a previous attempt that did not finish
    fs::create_directories(stage / "assets");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { if (!path.empty()) { std::error_code ignored; fs::remove_all(path, ignored); } }
    } cleanup{stage};

    uint64_t done = 0;
    std::vector<uint8_t> buffer(1 << 20);
    const auto copy = [&](const std::string& name, uint64_t size, auto&& read_part) {
        const fs::path output = stage / "assets" / utf8_path(name);
        fs::create_directories(output.parent_path());
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + name);
        for (uint64_t offset = 0; offset < size;) {
            const size_t chunk = size_t((std::min)(uint64_t(buffer.size()), size - offset));
            read_part(offset, std::span<uint8_t>(buffer.data(), chunk));
            if (!out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(chunk)))
                throw std::runtime_error("cannot write " + name + " (is the drive full?)");
            offset += chunk;
            done += chunk;
            if (!progress(InstallStage::copying, done, summary.total_bytes, name)) throw InstallCancelled();
        }
        if (!out.flush()) throw std::runtime_error("cannot write " + name);
    };

    if (summary.kind == SourceKind::disc_image) {
        DiscImage disc(source);
        for (const auto& entry : disc.entries()) {
            if (entry.directory) {
                fs::create_directories(stage / "assets" / utf8_path(entry.path));
                continue;
            }
            copy(entry.path, entry.size, [&](uint64_t offset, std::span<uint8_t> out) { disc.read(entry, offset, out); });
        }
    } else {
        for (const auto& [name, size] : folder_files(source)) {
            std::ifstream in(source / utf8_path(name), std::ios::binary);
            if (!in) throw std::runtime_error("cannot read " + name);
            copy(name, size, [&, name = name](uint64_t, std::span<uint8_t> out) {
                if (!in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())))
                    throw std::runtime_error("cannot read " + name);
            });
        }
    }

    if (!progress(InstallStage::decoding, 0, 1, "default.xex")) throw InstallCancelled();
    const auto xex = read_whole(stage / "assets" / "default.xex", supported_xex_size);
    if (!is_supported_xex(xex)) throw std::runtime_error("default.xex changed while it was copied");
    write_image_dump(xex, stage / "image");

    if (!progress(InstallStage::finishing, 0, 1, {})) throw InstallCancelled();
    for (const char* part : {"assets", "image"}) {
        fs::remove_all(game_directory / part, error);
        if (error) throw std::runtime_error(std::string("cannot replace the previous ") + part + " folder");
        fs::rename(stage / part, game_directory / part, error);
        if (error) throw std::runtime_error(std::string("cannot move the ") + part + " folder into place");
    }
    progress(InstallStage::finishing, 1, 1, {});
}
}
