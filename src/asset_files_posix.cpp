// AssetFiles on POSIX hosts (asset_files.cpp is the Windows one). Guest paths
// are case-insensitive, as on the console and NTFS, so each component is
// matched against its directory's entries ignoring ASCII case. Symbolic
// links are refused and every path stays under the mounted root.
// Asynchronous reads complete when submitted.
#include "asset_files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace sfr {
namespace {
constexpr uint32_t status_success = 0, status_end_of_file = 0xc0000011u, status_invalid_handle = 0xc0000008u,
                   status_name_not_found = 0xc0000034u, status_path_not_found = 0xc000003au,
                   status_file_is_a_directory = 0xc00000bau, status_invalid_parameter = 0xc000000du;
constexpr uint32_t attribute_directory = 0x10, attribute_archive = 0x20;

[[noreturn]] void native_error(const char* operation, int error) {
    throw std::runtime_error(std::string("asset files: ") + operation + " failed with error " + std::to_string(error));
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return out;
}

// The components of a game:/ path, checked like the Windows implementation.
std::vector<std::string> checked_components(std::string_view path) {
    auto separator = [](char value) { return value == '/' || value == '\\'; };
    if (path.size() <= 6 || lower(path.substr(0, 5)) != "game:" || !separator(path[5]))
        throw std::runtime_error("asset files: expected a nonempty game:/ mounted path");
    std::vector<std::string> components;
    size_t start = 6;
    for (size_t i = 6; i <= path.size(); ++i) {
        if (i == path.size() || separator(path[i])) {
            const auto component = path.substr(start, i - start);
            if (component.empty() || component == "." || component == ".." ||
                component.back() == '.' || component.back() == ' ')
                throw std::runtime_error("asset files: empty, traversal or trailing-dot/space path component");
            components.emplace_back(component);
            start = i + 1;
            continue;
        }
        const auto byte = static_cast<unsigned char>(path[i]);
        if (byte < 0x20 || byte >= 0x7f || path[i] == ':' || path[i] == '*' || path[i] == '?' ||
            path[i] == '<' || path[i] == '>' || path[i] == '"' || path[i] == '|')
            throw std::runtime_error("asset files: unsupported character in guest path");
    }
    return components;
}

// FILETIME (100 ns since 1601) of a POSIX time.
uint64_t filetime(const timespec& time) {
    return uint64_t(time.tv_sec) * 10000000ull + uint64_t(time.tv_nsec) / 100 + 116444736000000000ull;
}

AssetFiles::NetworkInformation information_of(const struct stat& info) {
    const bool directory = S_ISDIR(info.st_mode);
    return {filetime(info.st_mtim), filetime(info.st_atim), filetime(info.st_mtim), filetime(info.st_ctim),
            uint64_t(info.st_blocks) * 512, directory ? 0 : uint64_t(info.st_size),
            directory ? attribute_directory : attribute_archive};
}

struct NativeFileEntry {
    int fd;
    AssetFiles::OpenMode mode;
    uint64_t position = 0;
    std::atomic_uint32_t requests{0};
    NativeFileEntry(int descriptor, AssetFiles::OpenMode open_mode) : fd(descriptor), mode(open_mode) {}
    ~NativeFileEntry() { if (fd >= 0) ::close(fd); }
};
}

struct AssetFiles::Impl {
    std::string root;  // absolute, without a trailing slash
    std::map<uint32_t, std::shared_ptr<NativeFileEntry>> files;
    uint64_t next_handle = 0x72000004;
    // Lower-case name -> actual name, per directory (relative, '/'-joined).
    mutable std::mutex listing_mutex;
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>> listings;

    explicit Impl(const std::filesystem::path& path) {
        char* resolved = realpath(path.c_str(), nullptr);
        if (!resolved) native_error("mount asset root", errno);
        root = resolved;
        std::free(resolved);
        struct stat info{};
        if (stat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
            throw std::runtime_error("asset files: mount root must be a directory");
    }

    const std::unordered_map<std::string, std::string>& listing(const std::string& relative) const {
        std::lock_guard lock(listing_mutex);
        auto found = listings.find(relative);
        if (found != listings.end()) return found->second;
        auto& names = listings[relative];
        const std::string directory = relative.empty() ? root : root + "/" + relative;
        if (DIR* handle = opendir(directory.c_str())) {
            while (const dirent* entry = readdir(handle)) {
                const std::string name = entry->d_name;
                if (name != "." && name != "..") names.emplace(lower(name), name);
            }
            closedir(handle);
        }
        return names;
    }

    // The host path of a guest path, or the NT status of the failed lookup.
    std::pair<uint32_t, std::string> resolve(std::string_view guest_path) const {
        const auto components = checked_components(guest_path);
        std::string relative;
        for (size_t i = 0; i < components.size(); ++i) {
            const auto& names = listing(relative);
            const auto found = names.find(lower(components[i]));
            if (found == names.end())
                return {i + 1 == components.size() ? status_name_not_found : status_path_not_found, {}};
            relative += (relative.empty() ? "" : "/") + found->second;
        }
        const std::string host = root + "/" + relative;
        struct stat info{};
        if (lstat(host.c_str(), &info) != 0) return {status_name_not_found, {}};
        if (S_ISLNK(info.st_mode)) throw std::runtime_error("asset files: symbolic links are not followed");
        return {status_success, host};
    }
};

struct AssetFiles::AsyncRead::Impl {
    enum class State { prepared, terminal };
    std::shared_ptr<NativeFileEntry> file;
    std::vector<uint8_t> buffer;
    uint32_t length;
    uint64_t offset;
    State state = State::prepared;
    uint32_t status = 0, transferred = 0;

    Impl(std::shared_ptr<NativeFileEntry> retained, uint32_t requested, uint64_t file_offset)
        : file(std::move(retained)), buffer(requested), length(requested), offset(file_offset) {
        ++file->requests;
    }
    ~Impl() { --file->requests; }

    AsyncReadResult view() const {
        if (state != State::terminal) throw std::runtime_error("asset files: asynchronous read is not terminal");
        return {status, transferred, offset, std::span<const uint8_t>(buffer.data(), transferred)};
    }
};

AssetFiles::AsyncRead::AsyncRead(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AssetFiles::AsyncRead::~AsyncRead() = default;
AssetFiles::AsyncRead::AsyncRead(AsyncRead&&) noexcept = default;
AssetFiles::AsyncRead& AssetFiles::AsyncRead::operator=(AsyncRead&&) noexcept = default;

AssetFiles::SubmitResult AssetFiles::AsyncRead::submit() {
    if (!impl_ || impl_->state != Impl::State::prepared)
        throw std::runtime_error("asset files: asynchronous read may be submitted exactly once");
    size_t done = 0;
    while (done < impl_->length) {
        const ssize_t got = pread(impl_->file->fd, impl_->buffer.data() + done, impl_->length - done,
                                  off_t(impl_->offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            native_error("read native asset", errno);
        }
        if (got == 0) break;
        done += size_t(got);
    }
    impl_->transferred = uint32_t(done);
    impl_->status = done || impl_->length == 0 ? status_success : status_end_of_file;
    impl_->state = Impl::State::terminal;
    return SubmitResult::complete;
}

AssetFiles::AsyncReadResult AssetFiles::AsyncRead::wait(std::stop_token) {
    if (!impl_ || impl_->state == Impl::State::prepared)
        throw std::runtime_error("asset files: asynchronous read must be submitted before waiting");
    return impl_->view();
}

AssetFiles::AsyncReadResult AssetFiles::AsyncRead::result() const {
    if (!impl_) throw std::runtime_error("asset files: moved asynchronous read request");
    return impl_->view();
}

AssetFiles::AssetFiles(const std::filesystem::path& root) : impl_(std::make_unique<Impl>(root)) {}
AssetFiles::~AssetFiles() = default;

AssetFiles::OpenResult AssetFiles::open(std::string_view guest_path, uint32_t share_access, OpenMode mode) {
    if (share_access & ~uint32_t(7)) throw std::runtime_error("asset files: unsupported native sharing bits");
    if (impl_->next_handle >= 0x72100000)
        throw std::runtime_error("asset files: guest file handle namespace exhausted");
    const auto [status, host] = impl_->resolve(guest_path);
    if (status) return {status, 0xffffffff, 0};
    const int fd = ::open(host.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return {errno == ENOENT ? status_name_not_found : 0xc0000022u, 0xffffffff, 0};
    struct stat info{};
    if (fstat(fd, &info) != 0) {
        ::close(fd);
        native_error("query native file", errno);
    }
    if (S_ISDIR(info.st_mode)) {
        ::close(fd);
        return {status_file_is_a_directory, 0xffffffff, 0};
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        throw std::runtime_error("asset files: only regular files are supported");
    }
    const auto handle = static_cast<uint32_t>(impl_->next_handle);
    impl_->files.emplace(handle, std::make_shared<NativeFileEntry>(fd, mode));
    impl_->next_handle += 4;
    return {status_success, handle, uint64_t(info.st_size)};
}

AssetFiles::PathInformation AssetFiles::query_path(std::string_view guest_path) const {
    const auto [status, host] = impl_->resolve(guest_path);
    if (status) return {status, {}};
    struct stat info{};
    if (stat(host.c_str(), &info) != 0) return {status_name_not_found, {}};
    return {status_success, information_of(info)};
}

uint32_t AssetFiles::set_position(uint32_t handle, uint64_t offset) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) return status_invalid_handle;
    if (found->second->mode != OpenMode::synchronous)
        throw std::runtime_error("asset files: asynchronous handles have no file position");
    if (offset > uint64_t(std::numeric_limits<int64_t>::max())) return status_invalid_parameter;
    found->second->position = offset;
    return status_success;
}

uint64_t AssetFiles::size(uint32_t handle) const {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) throw std::runtime_error("asset files: invalid size handle");
    struct stat info{};
    if (fstat(found->second->fd, &info) != 0) native_error("query native file size", errno);
    return uint64_t(info.st_size);
}

AssetFiles::ReadResult AssetFiles::read(uint32_t handle, uint32_t length, std::optional<uint64_t> offset) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) return {status_invalid_handle, {}, 0};
    auto& file = *found->second;
    if (file.mode != OpenMode::synchronous)
        throw std::runtime_error("asset files: synchronous read is unavailable for an asynchronous handle");
    constexpr uint64_t use_current_position = std::numeric_limits<uint64_t>::max() - 1;
    const bool explicit_offset = offset && *offset != use_current_position;
    if (explicit_offset && *offset > uint64_t(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("asset files: unsupported signed-negative read offset");
    std::vector<uint8_t> bytes(length);
    if (length && explicit_offset) file.position = *offset;
    const uint64_t start = file.position;
    if (!length) return {status_success, {}, start};
    size_t done = 0;
    while (done < length) {
        const ssize_t got = pread(file.fd, bytes.data() + done, length - done, off_t(start + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            native_error("read native asset", errno);
        }
        if (got == 0) break;
        done += size_t(got);
    }
    bytes.resize(done);
    file.position = start + done;
    return {done ? status_success : status_end_of_file, std::move(bytes), start};
}

std::optional<AssetFiles::NetworkInformation> AssetFiles::network_information(uint32_t handle) const {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) return std::nullopt;
    struct stat info{};
    if (fstat(found->second->fd, &info) != 0) native_error("query native file information", errno);
    return information_of(info);
}

// What Windows reports for the same opens (GENERIC_READ, synchronous or not,
// buffered or not); used only in logs.
AssetFiles::NativeInformation AssetFiles::native_information(uint32_t handle) const {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) throw std::runtime_error("asset files: invalid native information handle");
    const auto mode = found->second->mode;
    return {0x120089u, mode == OpenMode::synchronous ? 0x20u : mode == OpenMode::asynchronous_unbuffered ? 0x8u : 0u, 0};
}

std::unique_ptr<AssetFiles::AsyncRead> AssetFiles::prepare_async_read(uint32_t handle, uint32_t length,
                                                                      uint64_t offset) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) throw std::runtime_error("asset files: invalid asynchronous read handle");
    if (found->second->mode == OpenMode::synchronous)
        throw std::runtime_error("asset files: asynchronous read requires asynchronous file mode");
    if (offset > uint64_t(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("asset files: asynchronous read offset exceeds signed file range");
    // The console's unbuffered reads come in whole sectors.
    if (found->second->mode == OpenMode::asynchronous_unbuffered && (offset % 512 || length % 512))
        throw std::runtime_error("asset files: unbuffered offset and length require sector alignment");
    auto result = std::unique_ptr<AsyncRead>(new AsyncRead(nullptr));
    result->impl_ = std::make_unique<AsyncRead::Impl>(found->second, length, offset);
    return result;
}

bool AssetFiles::owns(uint32_t handle) const { return impl_->files.find(handle) != impl_->files.end(); }

void AssetFiles::close(uint32_t handle) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) throw std::runtime_error("asset files: invalid close handle");
    if (found->second->requests.load() != 0)
        throw std::runtime_error("asset files: cannot close a file with an active asynchronous read");
    impl_->files.erase(found);
}

size_t AssetFiles::open_count() const { return impl_->files.size(); }
}
