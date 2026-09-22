#include "asset_files.h"
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Mode = sfr::AssetFiles::OpenMode;
constexpr uint32_t invalid_handle = 0xFFFFFFFF;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation, const char* message) {
    try { operation(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit NativeHandle(HANDLE handle) : value(handle) {}
    ~NativeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
};
struct Fixture {
    std::filesystem::path directory, mount;
    std::vector<uint8_t> payload;
    Fixture() : payload(14774) {
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 37 + 11);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                ("sfr-async-assets-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(GetTickCount64()) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) { directory = candidate; break; }
        }
        require(!directory.empty(), "create isolated native asset fixture");
        try {
            mount = directory / "mount";
            std::filesystem::create_directories(mount / "subdir");
            std::filesystem::create_directory(directory / "mount-escape");
            write(mount / "FNT_SE");
            write(mount / "subdir" / "different.bin");
            write(directory / "mount-escape" / "outside.bin");
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
            throw;
        }
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    void write(const std::filesystem::path& path) const {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        require(bool(stream), "write synthetic native asset bytes");
    }
    std::vector<uint8_t> bytes(const std::filesystem::path& path) const {
        std::ifstream stream(path, std::ios::binary);
        require(bool(stream), "reopen fixture for independent content verification");
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
};

uint32_t native_word(HANDLE handle, unsigned information_class) {
    using Query = LONG (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                             "NtQueryInformationFile"));
    require(query != nullptr, "Windows NtQueryInformationFile is available for independent mode evidence");
    IO_STATUS_BLOCK io{};
    ULONG value = 0;
    const LONG status = query(handle, &io, &value, sizeof(value),
                              static_cast<FILE_INFORMATION_CLASS>(information_class));
    require(status == 0 && io.Information == sizeof(value), "query actual Windows access/mode information");
    return value;
}
void require_actual_native_information(const sfr::AssetFiles::NativeInformation& actual, HANDLE reference,
                                       bool asynchronous) {
    // FileAccessInformation=8 and FileModeInformation=16 are NT classes;
    // neither is a GetFileInformationByHandleEx FILE_INFO_BY_HANDLE_CLASS value.
    require(actual.access_flags == native_word(reference, 8) && actual.mode == native_word(reference, 16),
            "reported access and mode must match independently queried real Windows handle state");
    FILE_ALIGNMENT_INFO alignment{};
    require(GetFileInformationByHandleEx(reference, FileAlignmentInfo, &alignment, sizeof(alignment)) != FALSE &&
                actual.alignment_requirement == alignment.AlignmentRequirement,
            "alignment requirement comes from the actual filesystem handle");
    require((actual.access_flags & (FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL)) ==
                (FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL) &&
                !(actual.access_flags & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
                                         FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER)),
            "native asset handle grants read access without any write/delete rights");
    constexpr uint32_t no_buffering = 8, synchronous_alert = 16, synchronous_nonalert = 32;
    if (asynchronous)
        require((actual.mode & no_buffering) && !(actual.mode & (synchronous_alert | synchronous_nonalert)),
                "actual async mode is unbuffered and has neither synchronous I/O bit");
    else
        require(!(actual.mode & no_buffering) && (actual.mode & synchronous_nonalert),
                "existing synchronous open remains buffered with real synchronous native mode");
}

void opens_actual_overlapped_unbuffered_readonly_handles_for_generic_paths() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto opened = files.open("game:\\FNT_SE", 1, Mode::asynchronous_unbuffered);
    require(opened.status == 0 && opened.handle == 0x72000004 && opened.size == 14774 &&
                files.open_count() == 1 && files.size(opened.handle) == fixture.payload.size(),
            "requested async mode opens the existing synthetic 14774-byte asset with an owned handle");
    NativeHandle reference(CreateFileW((fixture.mount / "FNT_SE").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, nullptr));
    require(reference.value != INVALID_HANDLE_VALUE, "independent native async reference opens");
    require_actual_native_information(files.native_information(opened.handle), reference.value, true);
    const auto metadata = files.network_information(opened.handle);
    require(metadata && metadata->end_of_file == 14774 && !(metadata->attributes & FILE_ATTRIBUTE_DIRECTORY),
            "async file retains real non-directory metadata");
    const auto different = files.open("GaMe:/SUBDIR/DIFFERENT.BIN", 1, Mode::asynchronous_unbuffered);
    require(different.status == 0 && different.handle == opened.handle + 4 && different.size == opened.size,
            "asynchronous mode is driven by caller intent rather than a special filename");
    require_actual_native_information(files.native_information(different.handle), reference.value, true);
    files.close(opened.handle);
    rejects([&] { (void)files.native_information(opened.handle); }, "closed async handle has no native information");
    require(!files.network_information(opened.handle) && files.open_count() == 1,
            "async close removes ownership and metadata access");
    files.close(different.handle);
    require(files.open_count() == 0 && fixture.bytes(fixture.mount / "FNT_SE") == fixture.payload,
            "async opens and information queries preserve original file content");
}

void async_handles_never_use_synchronous_read_and_old_mode_stays_unchanged() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto asynchronous = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    const auto synchronous = files.open("game:/FNT_SE", 1);
    require(asynchronous.status == 0 && synchronous.status == 0, "async and old sync opens coexist");
    NativeHandle reference(CreateFileW((fixture.mount / "FNT_SE").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    require(reference.value != INVALID_HANDLE_VALUE, "independent old-mode native reference opens");
    require_actual_native_information(files.native_information(synchronous.handle), reference.value, false);
    for (uint32_t length : {0u, 1u, 4096u}) {
        rejects([&] { (void)files.read(asynchronous.handle, length); },
                "async handle must reject current-position synchronous read, including zero length");
        rejects([&] { (void)files.read(asynchronous.handle, length, 0); },
                "async handle must reject explicit-offset synchronous read before native I/O");
    }
    const auto first = files.read(synchronous.handle, 3);
    const auto next = files.read(synchronous.handle, 4);
    require(first.status == 0 && first.offset == 0 && next.status == 0 && next.offset == 3 &&
                first.bytes == std::vector<uint8_t>(fixture.payload.begin(), fixture.payload.begin() + 3) &&
                next.bytes == std::vector<uint8_t>(fixture.payload.begin() + 3, fixture.payload.begin() + 7),
            "rejected async reads preserve existing synchronous native read bytes and position behavior");
    const auto explicit_sync = files.open("game:/subdir/different.bin", 1, Mode::synchronous);
    require(explicit_sync.status == 0, "explicit synchronous mode matches the legacy default");
    require_actual_native_information(files.native_information(explicit_sync.handle), reference.value, false);
    require(files.open_count() == 3 && fixture.bytes(fixture.mount / "FNT_SE") == fixture.payload,
            "rejected sync reads on async handles neither close handles nor modify assets");
}

void real_windows_sharing_and_failed_opens_preserve_ownership() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    {
        NativeHandle exclusive(CreateFileW((fixture.mount / "FNT_SE").c_str(), GENERIC_READ, 0,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(exclusive.value != INVALID_HANDLE_VALUE, "independent exclusive native reader opens");
        const auto blocked = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
        require(blocked.status == 0xC0000043 && blocked.handle == invalid_handle && blocked.size == 0 &&
                    files.open_count() == 0, "async open reports actual host sharing conflict without a guest handle");
    }
    const auto missing = files.open("game:/missing.bin", 1, Mode::asynchronous_unbuffered);
    const auto missing_path = files.open("game:/absent/file.bin", 1, Mode::asynchronous_unbuffered);
    require(missing.status == 0xC0000034 && missing_path.status == 0xC000003A &&
                missing.handle == invalid_handle && missing_path.handle == invalid_handle,
            "async existing-only open preserves native name/path-not-found distinctions");
    const auto directory = files.open("game:/subdir", 1, Mode::asynchronous_unbuffered);
    require(directory.status != 0 && directory.handle == invalid_handle && directory.size == 0,
            "async non-directory profile never publishes a directory handle");
    const auto opened = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    require(opened.status == 0 && opened.handle == 0x72000004 && files.open_count() == 1,
            "all failed native opens preserve the first guest handle ID");
    {
        NativeHandle reader(CreateFileW((fixture.mount / "FNT_SE").c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(reader.value != INVALID_HANDLE_VALUE, "share1 permits another native reader");
        NativeHandle writer(CreateFileW((fixture.mount / "FNT_SE").c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
        const DWORD write_error = GetLastError();
        require(writer.value == INVALID_HANDLE_VALUE && write_error == ERROR_SHARING_VIOLATION,
                "share1 actually denies a native writer");
        NativeHandle remover(CreateFileW((fixture.mount / "FNT_SE").c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
        const DWORD delete_error = GetLastError();
        require(remover.value == INVALID_HANDLE_VALUE && delete_error == ERROR_SHARING_VIOLATION,
                "share1 actually denies native delete access");
    }
    files.close(opened.handle);
    NativeHandle exclusive(CreateFileW((fixture.mount / "FNT_SE").c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(exclusive.value != INVALID_HANDLE_VALUE, "closing async handle releases its actual native sharing claim");
}

void invalid_intents_paths_and_handles_do_not_create_files_or_ids() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    for (std::string_view path : {"game:/../mount-escape/outside.bin", "game:/subdir/../FNT_SE",
                                  "game:/FNT_SE:stream", "C:/FNT_SE", "game:/", "game://FNT_SE"})
        rejects([&] { (void)files.open(path, 1, Mode::asynchronous_unbuffered); },
                "asynchronous open retains strict mounted-path validation");
    rejects([&] { (void)files.open("game:/FNT_SE", 8, Mode::asynchronous_unbuffered); },
            "async mode does not admit unknown share bits");
    rejects([&] { (void)files.open("game:/FNT_SE", 1, static_cast<Mode>(0xFFFFFFFF)); },
            "unknown native caller mode rejects before opening a file");
    for (uint32_t handle : {0u, invalid_handle, 0x72000004u})
        rejects([&] { (void)files.native_information(handle); }, "unowned handles cannot yield fabricated native mode");
    require(files.open_count() == 0 && !std::filesystem::exists(fixture.mount / "missing.bin"),
            "invalid intents and paths leave no retained handles or new files");
    const auto opened = files.open("game:/FNT_SE", 1, Mode::asynchronous_unbuffered);
    require(opened.status == 0 && opened.handle == 0x72000004,
            "validation failures preserve guest file handle allocation");
}

void make_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::filesystem::create_directory(link);
    NativeHandle handle(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    require(handle.value != INVALID_HANDLE_VALUE, "open junction fixture directory");
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const auto length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    struct Junction {
        DWORD tag;
        WORD size, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t names[2048];
    } data{};
    require(substitute.size() + 2 < 2048, "junction target fits fixture buffer");
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.size = 8 + length + 4;
    data.substitute_length = length;
    data.print_offset = length + 2;
    std::memcpy(data.names, substitute.data(), length);
    DWORD returned = 0;
    require(DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT, &data, 8 + data.size,
                            nullptr, 0, &returned, nullptr) != FALSE, "create real junction fixture");
}

void asynchronous_open_checks_retained_native_path_after_reparse() {
    Fixture fixture;
    make_junction(fixture.mount / "escape", fixture.directory / "mount-escape");
    make_junction(fixture.mount / "inside", fixture.mount / "subdir");
    sfr::AssetFiles files(fixture.mount);
    rejects([&] { (void)files.open("game:/escape/outside.bin", 1, Mode::asynchronous_unbuffered); },
            "async native handle final path cannot escape through a junction or prefix-collision sibling");
    require(files.open_count() == 0, "escaped async handle is not retained");
    NativeHandle outside(CreateFileW((fixture.directory / "mount-escape" / "outside.bin").c_str(),
        GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(outside.value != INVALID_HANDLE_VALUE, "rejected async escape closes its native handle");
    const auto inside = files.open("game:/inside/different.bin", 1, Mode::asynchronous_unbuffered);
    require(inside.status == 0 && inside.handle == 0x72000004 && inside.size == fixture.payload.size(),
            "reparse target contained by the mounted directory remains supported");
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {opens_actual_overlapped_unbuffered_readonly_handles_for_generic_paths,
                      async_handles_never_use_synchronous_read_and_old_mode_stays_unchanged,
                      real_windows_sharing_and_failed_opens_preserve_ownership,
                      invalid_intents_paths_and_handles_do_not_create_files_or_ids,
                      asynchronous_open_checks_retained_native_path_after_reparse}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
