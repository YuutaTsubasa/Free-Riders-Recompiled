#include "asset_files.h"

#define NOMINMAX
#include <windows.h>

#include <limits>
#include <map>
#include <atomic>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace sfr {
namespace {
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;

    explicit NativeHandle(HANDLE handle) : value(handle) {}
    ~NativeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle(NativeHandle&& other) noexcept
        : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
};

[[noreturn]] void native_error(const char* operation, DWORD error) {
    throw std::runtime_error(std::string("asset files: ") + operation +
                             " failed with Windows error " + std::to_string(error));
}

std::wstring final_path(HANDLE handle) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (!needed) native_error("resolve final handle path", GetLastError());
    std::wstring path(needed, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), needed, flags);
    if (!written) native_error("resolve final handle path", GetLastError());
    if (written >= needed)
        throw std::runtime_error("asset files: final handle path changed while resolving it");
    path.resize(written);
    return path;
}

bool is_disk(HANDLE handle) {
    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(handle);
    if (type == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS)
        native_error("query native file type", GetLastError());
    return type == FILE_TYPE_DISK;
}

DWORD attributes(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
        native_error("query native file attributes", GetLastError());
    return info.dwFileAttributes;
}

uint64_t native_size(HANDLE handle) {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length)) native_error("query native file size", GetLastError());
    if (length.QuadPart < 0) throw std::runtime_error("asset files: native file size is negative");
    return static_cast<uint64_t>(length.QuadPart);
}

std::wstring checked_relative_path(std::string_view path) {
    auto lower_ascii = [](char value) {
        return value >= 'A' && value <= 'Z' ? char(value + ('a' - 'A')) : value;
    };
    auto separator = [](char value) { return value == '/' || value == '\\'; };
    if (path.size() <= 6 || lower_ascii(path[0]) != 'g' || lower_ascii(path[1]) != 'a' ||
        lower_ascii(path[2]) != 'm' || lower_ascii(path[3]) != 'e' || path[4] != ':' ||
        !separator(path[5]))
        throw std::runtime_error("asset files: expected a nonempty game:/ mounted path");

    std::wstring relative;
    relative.reserve(path.size() - 6);
    size_t component_start = 6;
    for (size_t i = 6; i <= path.size(); ++i) {
        if (i == path.size() || separator(path[i])) {
            const auto component = path.substr(component_start, i - component_start);
            if (component.empty() || component == "." || component == ".." ||
                component.back() == '.' || component.back() == ' ')
                throw std::runtime_error("asset files: empty, traversal or trailing-dot/space path component");
            if (i != path.size()) relative.push_back(L'\\');
            component_start = i + 1;
            continue;
        }
        const auto byte = static_cast<unsigned char>(path[i]);
        if (byte < 0x20 || byte >= 0x7f || path[i] == ':' || path[i] == '*' || path[i] == '?' ||
            path[i] == '<' || path[i] == '>' || path[i] == '"' || path[i] == '|')
            throw std::runtime_error("asset files: unsupported character in guest path");
        relative.push_back(static_cast<wchar_t>(byte));
    }
    return relative;
}

std::wstring directory_prefix(std::wstring root) {
    if (root.empty() || root.back() != L'\\') root.push_back(L'\\');
    return root;
}

bool contained_by(const std::wstring& path, const std::wstring& root_prefix) {
    if (path.size() <= root_prefix.size() ||
        root_prefix.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    return CompareStringOrdinal(path.data(), static_cast<int>(root_prefix.size()),
                                root_prefix.data(), static_cast<int>(root_prefix.size()), TRUE) == CSTR_EQUAL;
}

AssetFiles::OpenResult failed_open(DWORD error) {
    uint32_t status = 0;
    switch (error) {
    case ERROR_FILE_NOT_FOUND: status = 0xc0000034; break; // STATUS_OBJECT_NAME_NOT_FOUND
    case ERROR_PATH_NOT_FOUND: status = 0xc000003a; break; // STATUS_OBJECT_PATH_NOT_FOUND
    case ERROR_ACCESS_DENIED: status = 0xc0000022; break; // STATUS_ACCESS_DENIED
    case ERROR_SHARING_VIOLATION: status = 0xc0000043; break; // STATUS_SHARING_VIOLATION
    case ERROR_INVALID_NAME: status = 0xc0000033; break; // STATUS_OBJECT_NAME_INVALID
    default: native_error("open native asset", error);
    }
    return {status, 0xffffffff, 0};
}

struct NativeFileEntry {
    NativeHandle native;
    AssetFiles::OpenMode mode;
    std::atomic_uint32_t requests{0};

    NativeFileEntry(NativeHandle handle, AssetFiles::OpenMode open_mode)
        : native(std::move(handle)), mode(open_mode) {}
};

struct NativeIoStatusBlock {
    union { LONG status; void* pointer; };
    ULONG_PTR information;
};

using NtQueryInformationFileFunction = LONG(NTAPI*)(HANDLE, NativeIoStatusBlock*, void*, ULONG, int);

NtQueryInformationFileFunction nt_query_information_file() {
    static const auto function = reinterpret_cast<NtQueryInformationFileFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile"));
    if (!function) native_error("resolve NtQueryInformationFile", GetLastError());
    return function;
}

uint32_t query_native_word(HANDLE handle, int information_class, const char* operation) {
    uint32_t value = 0;
    NativeIoStatusBlock io{};
    const LONG status = nt_query_information_file()(handle, &io, &value, sizeof(value), information_class);
    if (status < 0 || io.information != sizeof(value)) {
        std::ostringstream detail;
        detail << operation << " failed with NT status 0x" << std::hex
               << static_cast<uint32_t>(status) << " and information " << std::dec << io.information;
        throw std::runtime_error("asset files: " + detail.str());
    }
    return value;
}
}

struct AssetFiles::Impl {
    NativeHandle root;
    std::map<uint32_t, std::shared_ptr<NativeFileEntry>> files;
    uint64_t next_handle = 0x72000004;

    explicit Impl(const std::filesystem::path& path)
        : root(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)) {
        if (root.value == INVALID_HANDLE_VALUE) native_error("mount asset root", GetLastError());
        if (!is_disk(root.value) || !(attributes(root.value) & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("asset files: mount root must be a disk directory");
        (void)final_path(root.value);
    }
};

struct AssetFiles::AsyncRead::Impl {
    enum class State { prepared, pending, terminal, failed };
    std::shared_ptr<NativeFileEntry> file;
    NativeHandle event;
    OVERLAPPED overlapped{};
    void* buffer = nullptr;
    uint32_t length;
    uint64_t offset;
    State state = State::prepared;
    uint32_t status = 0;
    uint32_t transferred = 0;
    DWORD failure = ERROR_SUCCESS;

    Impl(std::shared_ptr<NativeFileEntry> retained, HANDLE completion_event,
         void* staging, uint32_t requested, uint64_t file_offset)
        : file(std::move(retained)), event(completion_event), buffer(staging),
          length(requested), offset(file_offset) {
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        overlapped.hEvent = event.value;
        ++file->requests;
    }
    ~Impl() {
        if (state == State::pending) {
            if (!CancelIoEx(file->native.value, &overlapped) && GetLastError() != ERROR_NOT_FOUND)
                std::terminate();
            DWORD ignored = 0;
            // TRUE drains the request. Any reported operation error is terminal too.
            (void)GetOverlappedResult(file->native.value, &overlapped, &ignored, TRUE);
        }
        if (buffer && !VirtualFree(buffer, 0, MEM_RELEASE)) std::terminate();
        --file->requests;
    }

    AsyncReadResult view() const {
        if (state == State::failed) native_error("complete asynchronous read", failure);
        if (state != State::terminal)
            throw std::runtime_error("asset files: asynchronous read is not terminal");
        return {status, transferred, offset,
                std::span<const uint8_t>(static_cast<const uint8_t*>(buffer), transferred)};
    }

    void finish(DWORD bytes, DWORD error) {
        transferred = bytes;
        if (error == ERROR_SUCCESS) status = bytes || length == 0 ? 0u : 0xc0000011u;
        else if (error == ERROR_HANDLE_EOF) status = 0xc0000011u;
        else if (error == ERROR_OPERATION_ABORTED) status = 0xc0000120u;
        else {
            failure = error;
            state = State::failed;
            native_error("complete asynchronous read", error);
        }
        state = State::terminal;
    }
};

AssetFiles::AsyncRead::AsyncRead(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AssetFiles::AsyncRead::~AsyncRead() = default;
AssetFiles::AsyncRead::AsyncRead(AsyncRead&&) noexcept = default;
AssetFiles::AsyncRead& AssetFiles::AsyncRead::operator=(AsyncRead&&) noexcept = default;
AssetFiles::SubmitResult AssetFiles::AsyncRead::submit() {
    if (!impl_ || impl_->state != Impl::State::prepared)
        throw std::runtime_error("asset files: asynchronous read may be submitted exactly once");
    if (!impl_->length) {
        impl_->finish(0, ERROR_SUCCESS);
        return SubmitResult::complete;
    }
    if (ReadFile(impl_->file->native.value, impl_->buffer, impl_->length, nullptr,
                 &impl_->overlapped)) {
        DWORD transferred = 0;
        if (GetOverlappedResult(impl_->file->native.value, &impl_->overlapped,
                                &transferred, FALSE))
            impl_->finish(transferred, ERROR_SUCCESS);
        else
            impl_->finish(0, GetLastError());
        return SubmitResult::complete;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        impl_->state = Impl::State::pending;
        return SubmitResult::pending;
    }
    impl_->finish(0, error);
    return SubmitResult::complete;
}
AssetFiles::AsyncReadResult AssetFiles::AsyncRead::wait(std::stop_token stop) {
    if (!impl_ || impl_->state == Impl::State::prepared)
        throw std::runtime_error("asset files: asynchronous read must be submitted before waiting");
    if (impl_->state == Impl::State::terminal || impl_->state == Impl::State::failed)
        return impl_->view();
    std::stop_callback cancel(stop, [request = impl_.get()] {
        if (!CancelIoEx(request->file->native.value, &request->overlapped)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NOT_FOUND) std::terminate();
        }
    });
    DWORD transferred = 0;
    if (GetOverlappedResult(impl_->file->native.value, &impl_->overlapped, &transferred, TRUE))
        impl_->finish(transferred, ERROR_SUCCESS);
    else
        impl_->finish(0, GetLastError());
    return impl_->view();
}
AssetFiles::AsyncReadResult AssetFiles::AsyncRead::result() const {
    if (!impl_) throw std::runtime_error("asset files: moved asynchronous read request");
    return impl_->view();
}

AssetFiles::AssetFiles(const std::filesystem::path& root) : impl_(std::make_unique<Impl>(root)) {}
AssetFiles::~AssetFiles() = default;

AssetFiles::OpenResult AssetFiles::open(std::string_view guest_path, uint32_t share_access,
                                        OpenMode mode) {
    const auto relative = checked_relative_path(guest_path);
    if (share_access & ~uint32_t(7))
        throw std::runtime_error("asset files: unsupported native sharing bits");
    if (mode != OpenMode::synchronous && mode != OpenMode::asynchronous_unbuffered &&
        mode != OpenMode::asynchronous_buffered)
        throw std::runtime_error("asset files: unsupported native open mode");
    // 72100000..721FFFFF is reserved for native synchronization handles.
    if (impl_->next_handle >= 0x72100000)
        throw std::runtime_error("asset files: guest file handle namespace exhausted");
    DWORD sharing = 0;
    if (share_access & 1) sharing |= FILE_SHARE_READ;
    if (share_access & 2) sharing |= FILE_SHARE_WRITE;
    if (share_access & 4) sharing |= FILE_SHARE_DELETE;

    const auto path = directory_prefix(final_path(impl_->root.value)) + relative;
    const DWORD open_flags = mode == OpenMode::synchronous ? FILE_FLAG_BACKUP_SEMANTICS
        : mode == OpenMode::asynchronous_buffered ? FILE_FLAG_OVERLAPPED
        : FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING;
    // Synchronous BACKUP_SEMANTICS allows a directory handle so it can be rejected explicitly.
    // The asynchronous profile corresponds to the observed NON_DIRECTORY request.
    NativeHandle file(CreateFileW(path.c_str(), GENERIC_READ, sharing, nullptr,
                                  OPEN_EXISTING, open_flags, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return failed_open(GetLastError());
    if (!is_disk(file.value)) throw std::runtime_error("asset files: only disk files are supported");
    // Compare the actual retained objects after following reparse points, including a component boundary.
    const auto opened_path = final_path(file.value);
    const auto mounted_prefix = directory_prefix(final_path(impl_->root.value));
    if (!contained_by(opened_path, mounted_prefix))
        throw std::runtime_error("asset files: opened native file escapes the mounted root");
    if (attributes(file.value) & FILE_ATTRIBUTE_DIRECTORY) return {0xc00000ba, 0xffffffff, 0};

    const uint64_t length = native_size(file.value);
    const auto handle = static_cast<uint32_t>(impl_->next_handle);
    auto entry = std::make_shared<NativeFileEntry>(std::move(file), mode);
    impl_->files.emplace(handle, std::move(entry));
    impl_->next_handle += 4;
    return {0, handle, length};
}

AssetFiles::PathInformation AssetFiles::query_path(std::string_view guest_path) const {
    const auto relative = checked_relative_path(guest_path);
    const auto path = directory_prefix(final_path(impl_->root.value)) + relative;
    // BACKUP_SEMANTICS opens directories too; attributes only, no data access.
    NativeHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return {failed_open(GetLastError()).status, {}};
    if (!is_disk(file.value)) throw std::runtime_error("asset files: only disk files are supported");
    const auto opened_path = final_path(file.value);
    const auto mounted_prefix = directory_prefix(final_path(impl_->root.value));
    if (!contained_by(opened_path, mounted_prefix))
        throw std::runtime_error("asset files: queried native path escapes the mounted root");
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)))
        native_error("query native basic information", GetLastError());
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)))
        native_error("query native standard information", GetLastError());
    if (standard.AllocationSize.QuadPart < 0 || standard.EndOfFile.QuadPart < 0)
        throw std::runtime_error("asset files: native allocation size or end of file is negative");
    return {0, NetworkInformation{
        static_cast<uint64_t>(basic.CreationTime.QuadPart),
        static_cast<uint64_t>(basic.LastAccessTime.QuadPart),
        static_cast<uint64_t>(basic.LastWriteTime.QuadPart),
        static_cast<uint64_t>(basic.ChangeTime.QuadPart),
        static_cast<uint64_t>(standard.AllocationSize.QuadPart),
        static_cast<uint64_t>(standard.EndOfFile.QuadPart),
        basic.FileAttributes}};
}

uint32_t AssetFiles::set_position(uint32_t handle, uint64_t offset) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) return 0xc0000008; // STATUS_INVALID_HANDLE
    if (found->second->mode != OpenMode::synchronous)
        throw std::runtime_error("asset files: asynchronous handles have no file position");
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return 0xc000000d; // STATUS_INVALID_PARAMETER
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<int64_t>(offset);
    if (!SetFilePointerEx(found->second->native.value, distance, nullptr, FILE_BEGIN))
        native_error("set native file position", GetLastError());
    return 0;
}

uint64_t AssetFiles::size(uint32_t handle) const {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) throw std::runtime_error("asset files: invalid size handle");
    return native_size(found->second->native.value);
}

AssetFiles::ReadResult AssetFiles::read(uint32_t handle, uint32_t length,
                                       std::optional<uint64_t> offset) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) return {0xc0000008, {}, 0}; // STATUS_INVALID_HANDLE
    if (found->second->mode != OpenMode::synchronous)
        throw std::runtime_error("asset files: synchronous read is unavailable for an asynchronous handle");

    constexpr uint64_t use_current_position = std::numeric_limits<uint64_t>::max() - 1;
    const bool explicit_offset = offset && *offset != use_current_position;
    if (explicit_offset && *offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("asset files: unsupported signed-negative read offset");

    // Allocate before any seek so allocation failure cannot advance the native position.
    std::vector<uint8_t> bytes(length);
    LARGE_INTEGER distance{};
    DWORD origin = FILE_CURRENT;
    // A zero-length read is a no-op even when a different valid offset was supplied.
    if (length && explicit_offset) {
        distance.QuadPart = static_cast<int64_t>(*offset);
        origin = FILE_BEGIN;
    }
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(found->second->native.value, distance, &position, origin))
        native_error("set or query native read position", GetLastError());
    if (position.QuadPart < 0)
        throw std::runtime_error("asset files: native read position is negative");
    const auto start = static_cast<uint64_t>(position.QuadPart);
    if (!length) return {0, {}, start};

    DWORD transferred = 0;
    if (!ReadFile(found->second->native.value, bytes.data(), length, &transferred, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF) return {0xc0000011, {}, start}; // STATUS_END_OF_FILE
        native_error("read native asset", error);
    }
    bytes.resize(transferred);
    return {transferred ? 0u : 0xc0000011u, std::move(bytes), start};
}

std::optional<AssetFiles::NetworkInformation> AssetFiles::network_information(uint32_t handle) const {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) return std::nullopt;

    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(found->second->native.value, FileBasicInfo, &basic, sizeof(basic)))
        native_error("query native basic information", GetLastError());
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(found->second->native.value, FileStandardInfo, &standard, sizeof(standard)))
        native_error("query native standard information", GetLastError());
    if (standard.AllocationSize.QuadPart < 0 || standard.EndOfFile.QuadPart < 0)
        throw std::runtime_error("asset files: native allocation size or end of file is negative");

    return NetworkInformation{
        static_cast<uint64_t>(basic.CreationTime.QuadPart),
        static_cast<uint64_t>(basic.LastAccessTime.QuadPart),
        static_cast<uint64_t>(basic.LastWriteTime.QuadPart),
        static_cast<uint64_t>(basic.ChangeTime.QuadPart),
        static_cast<uint64_t>(standard.AllocationSize.QuadPart),
        static_cast<uint64_t>(standard.EndOfFile.QuadPart),
        basic.FileAttributes};
}

AssetFiles::NativeInformation AssetFiles::native_information(uint32_t handle) const {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end())
        throw std::runtime_error("asset files: invalid native information handle");
    constexpr int file_access_information = 8;
    constexpr int file_mode_information = 16;
    FILE_ALIGNMENT_INFO alignment{};
    if (!GetFileInformationByHandleEx(found->second->native.value, FileAlignmentInfo,
                                      &alignment, sizeof(alignment)))
        native_error("query native file alignment", GetLastError());
    return {query_native_word(found->second->native.value, file_access_information,
                              "query native file access"),
            query_native_word(found->second->native.value, file_mode_information,
                              "query native file mode"),
            alignment.AlignmentRequirement};
}

std::unique_ptr<AssetFiles::AsyncRead> AssetFiles::prepare_async_read(
    uint32_t handle, uint32_t length, uint64_t offset) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end())
        throw std::runtime_error("asset files: invalid asynchronous read handle");
    if (found->second->mode == OpenMode::synchronous)
        throw std::runtime_error("asset files: asynchronous read requires asynchronous file mode");
    const bool unbuffered = found->second->mode == OpenMode::asynchronous_unbuffered;
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("asset files: asynchronous read offset exceeds signed file range");

    const auto path = final_path(found->second->native.value);
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH))
        native_error("resolve asynchronous read volume", GetLastError());
    DWORD sectors_per_cluster = 0, sector = 0, free_clusters = 0, total_clusters = 0;
    if (!GetDiskFreeSpaceW(volume, &sectors_per_cluster, &sector, &free_clusters, &total_clusters))
        native_error("query asynchronous read sector size", GetLastError());
    if (unbuffered && (!sector || offset % sector || length % sector))
        throw std::runtime_error("asset files: unbuffered offset and length require sector alignment");

    NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.value == INVALID_HANDLE_VALUE || !event.value) {
        event.value = INVALID_HANDLE_VALUE;
        native_error("create asynchronous read event", GetLastError());
    }
    void* buffer = nullptr;
    if (length) {
        buffer = VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!buffer) native_error("allocate asynchronous read staging", GetLastError());
        FILE_ALIGNMENT_INFO alignment{};
        if (!GetFileInformationByHandleEx(found->second->native.value, FileAlignmentInfo,
                                          &alignment, sizeof(alignment))) {
            const DWORD error = GetLastError();
            VirtualFree(buffer, 0, MEM_RELEASE);
            native_error("query asynchronous read buffer alignment", error);
        }
        if (reinterpret_cast<uintptr_t>(buffer) & alignment.AlignmentRequirement) {
            VirtualFree(buffer, 0, MEM_RELEASE);
            throw std::runtime_error("asset files: native staging allocation does not meet file alignment");
        }
    }
    try {
        // Allocate the public owner before transferring any native resource or
        // incrementing the file's active-request count.
        auto result = std::unique_ptr<AsyncRead>(new AsyncRead(nullptr));
        auto request = std::make_unique<AsyncRead::Impl>(found->second, event.value, buffer, length, offset);
        event.value = INVALID_HANDLE_VALUE;
        buffer = nullptr;
        result->impl_ = std::move(request);
        return result;
    } catch (...) {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
        throw;
    }
}

bool AssetFiles::owns(uint32_t handle) const {
    return impl_->files.find(handle) != impl_->files.end();
}

void AssetFiles::close(uint32_t handle) {
    const auto found = impl_->files.find(handle);
    if (found == impl_->files.end()) throw std::runtime_error("asset files: invalid close handle");
    if (found->second->requests.load() != 0)
        throw std::runtime_error("asset files: cannot close a file with an active asynchronous read");
    if (!CloseHandle(found->second->native.value)) native_error("close native asset", GetLastError());
    found->second->native.value = INVALID_HANDLE_VALUE;
    impl_->files.erase(found);
}

size_t AssetFiles::open_count() const { return impl_->files.size(); }
}
