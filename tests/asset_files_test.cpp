#include "asset_files.h"

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F>
void rejects(F operation, const char* message) {
    try { operation(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}

struct NativeHandle {
    HANDLE value;
    ~NativeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct Fixture {
    std::filesystem::path directory;
    std::filesystem::path mount;
    const std::string bytes = std::string("shader\0fixture\xff", 15);

    Fixture() {
        const auto temporary = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = temporary / ("sfr-asset-files-" + std::to_string(GetCurrentProcessId()) +
                "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                directory = candidate;
                break;
            }
        }
        require(!directory.empty(), "could not create an isolated asset fixture");
        try {
            mount = directory / "mount";
            std::filesystem::create_directories(mount / "shader");
            std::filesystem::create_directories(directory / "mount-escape");
            write(mount / "shader" / "Xbox360BasicShader.fxobj");
            write(directory / "mount-escape" / "outside.fxobj");
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
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(bool(output), "fixture write failed");
    }

    std::string read() const {
        std::ifstream input(mount / "shader" / "Xbox360BasicShader.fxobj", std::ios::binary);
        require(bool(input), "fixture read failed");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
};

void opens_real_files_with_stable_identity() {
    Fixture fixture;
    {
        sfr::AssetFiles files(fixture.mount);
        require(files.open_count() == 0, "mount begins without file handles");
        const auto first = files.open("game:\\shader\\Xbox360BasicShader.fxobj", 1);
        require(first.status == 0 && first.handle == 0x72000004 && first.size == fixture.bytes.size(),
                "first open returns native size and the first opaque handle");
        require(files.size(first.handle) == fixture.bytes.size() && files.open_count() == 1,
                "opened file remains owned and queryable");
        const auto second = files.open("GaMe:/SHADER/xbox360basicshader.FXOBJ", 1);
        require(second.status == 0 && second.handle == first.handle + 4 && second.size == first.size,
                "mount and Windows path are case insensitive");
        files.close(first.handle);
        require(files.open_count() == 1, "closing one handle preserves other opens");
        rejects([&] { files.size(first.handle); }, "closed size handle must be rejected");
        rejects([&] { files.close(first.handle); }, "double close must be rejected");
        files.close(second.handle);
        const auto third = files.open("game:/shader/Xbox360BasicShader.fxobj", 0);
        require(third.status == 0 && third.handle == second.handle + 4,
                "reopen never reuses a guest handle");
    }
    NativeHandle exclusive{CreateFileW((fixture.mount / "shader" / "Xbox360BasicShader.fxobj").c_str(),
        GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(exclusive.value != INVALID_HANDLE_VALUE, "destruction closes native handles");
}

void require_read(const sfr::AssetFiles::ReadResult& result, uint32_t status,
                  uint64_t offset, std::string_view expected) {
    require(result.status == status, "read returns the expected NT status");
    require(result.offset == offset, "read reports the actual native starting position");
    const std::vector<uint8_t> bytes(expected.begin(), expected.end());
    require(result.bytes == bytes, "read preserves the exact transferred binary bytes");
}

void reads_native_bytes_and_preserves_independent_positions() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto first = files.open("game:/shader/Xbox360BasicShader.fxobj", 1);
    const auto second = files.open("game:/shader/Xbox360BasicShader.fxobj", 1);
    require(first.status == 0 && second.status == 0, "read fixtures open");
    const std::string_view expected(fixture.bytes);
    require_read(files.read(first.handle, 7), 0, 0, expected.substr(0, 7));
    require_read(files.read(first.handle, 3), 0, 7, expected.substr(7, 3));
    require_read(files.read(second.handle, 2), 0, 0, expected.substr(0, 2));
    require_read(files.read(first.handle, 2, 12), 0, 12, expected.substr(12, 2));
    constexpr uint64_t current_position = std::numeric_limits<uint64_t>::max() - 1;
    require_read(files.read(first.handle, 1, current_position), 0, 14, expected.substr(14, 1));
    require_read(files.read(second.handle, 3, current_position), 0, 2, expected.substr(2, 3));
    require_read(files.read(first.handle, 15, 0), 0, 0, expected);
    require(files.open_count() == 2 && fixture.read() == fixture.bytes,
            "reads preserve file contents and handle ownership");
}

void reads_partial_eof_and_zero_without_unrequested_position_changes() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto opened = files.open("game:/shader/Xbox360BasicShader.fxobj", 1);
    require(opened.status == 0, "EOF fixture opens");
    const std::string_view expected(fixture.bytes);
    require_read(files.read(opened.handle, 100, 10), 0, 10, expected.substr(10));
    require_read(files.read(opened.handle, 1), 0xc0000011, 15, {});
    require_read(files.read(opened.handle, 0), 0, 15, {});
    require_read(files.read(opened.handle, 0, 2), 0, 15, {});
    require_read(files.read(opened.handle, 1), 0xc0000011, 15, {});
    require_read(files.read(opened.handle, 1, 100), 0xc0000011, 100, {});
    require_read(files.read(opened.handle, 0, 0), 0, 100, {});
    require_read(files.read(opened.handle, 1), 0xc0000011, 100, {});
    require_read(files.read(opened.handle, 2, 0), 0, 0, expected.substr(0, 2));
    require_read(files.read(opened.handle, 0, 8), 0, 2, {});
    require_read(files.read(opened.handle, 2), 0, 2, expected.substr(2, 2));
    require(fixture.read() == fixture.bytes, "partial, EOF and zero reads never modify native content");
}

void rejects_invalid_read_handles_and_negative_offsets_without_advancing() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto opened = files.open("game:/shader/Xbox360BasicShader.fxobj", 1);
    require(opened.status == 0, "invalid-read fixture opens");
    const std::string_view expected(fixture.bytes);
    require_read(files.read(opened.handle, 3), 0, 0, expected.substr(0, 3));
    for (const auto offset : {std::numeric_limits<uint64_t>::max(),
                              std::numeric_limits<uint64_t>::max() - 2,
                              uint64_t(1) << 63}) {
        rejects([&] { files.read(opened.handle, 1, offset); },
                "unsupported signed-negative read offset must be rejected");
        require_read(files.read(opened.handle, 0), 0, 3, {});
    }
    for (const uint32_t handle : {0u, 0xffffffffu, opened.handle + 4}) {
        require_read(files.read(handle, 1), 0xc0000008, 0, {});
        require_read(files.read(handle, 0), 0xc0000008, 0, {});
    }
    require_read(files.read(opened.handle, 2), 0, 3, expected.substr(3, 2));
    files.close(opened.handle);
    require_read(files.read(opened.handle, 1), 0xc0000008, 0, {});
    require(files.open_count() == 0 && fixture.read() == fixture.bytes,
            "invalid reads preserve file content and never create handles");
}

void require_native_information(const sfr::AssetFiles::NetworkInformation& actual, HANDLE native) {
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    require(GetFileInformationByHandleEx(native, FileBasicInfo, &basic, sizeof(basic)) != 0,
            "independent native basic-information query failed");
    require(GetFileInformationByHandleEx(native, FileStandardInfo, &standard, sizeof(standard)) != 0,
            "independent native standard-information query failed");
    require(actual.creation_time == static_cast<uint64_t>(basic.CreationTime.QuadPart),
            "network information preserves native creation time");
    require(actual.last_access_time == static_cast<uint64_t>(basic.LastAccessTime.QuadPart),
            "network information preserves native access time");
    require(actual.last_write_time == static_cast<uint64_t>(basic.LastWriteTime.QuadPart),
            "network information preserves native write time");
    require(actual.change_time == static_cast<uint64_t>(basic.ChangeTime.QuadPart),
            "network information preserves native change time");
    require(actual.allocation_size == static_cast<uint64_t>(standard.AllocationSize.QuadPart),
            "network information uses actual native allocation size");
    require(actual.end_of_file == static_cast<uint64_t>(standard.EndOfFile.QuadPart),
            "network information uses actual native end of file");
    require(actual.attributes == basic.FileAttributes,
            "network information preserves native attributes");
}

void queries_actual_native_network_information() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto opened = files.open("game:/shader/Xbox360BasicShader.fxobj", 1);
    require(opened.status == 0, "metadata fixture opens");
    NativeHandle native{CreateFileW((fixture.mount / "shader" / "Xbox360BasicShader.fxobj").c_str(),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(native.value != INVALID_HANDLE_VALUE, "independent metadata handle opens");
    const auto information = files.network_information(opened.handle);
    require(information.has_value(), "live handle supplies network information");
    require_native_information(*information, native.value);
    require(information->end_of_file == fixture.bytes.size(), "metadata matches fixture byte length");
    require(!files.network_information(0) && !files.network_information(0xffffffff) &&
            !files.network_information(opened.handle + 4), "unknown metadata handles return no value");
    files.close(opened.handle);
    require(!files.network_information(opened.handle), "closed metadata handle returns no value");
    require(files.open_count() == 0 && fixture.read() == fixture.bytes,
            "metadata queries preserve native content and handle ownership");
}

void queries_fresh_metadata_after_native_changes() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    const auto opened = files.open("game:/shader/Xbox360BasicShader.fxobj", 3);
    require(opened.status == 0, "metadata fixture opens with write sharing");
    const auto before = files.network_information(opened.handle);
    require(before.has_value(), "initial metadata is available");
    NativeHandle writer{CreateFileW((fixture.mount / "shader" / "Xbox360BasicShader.fxobj").c_str(),
        GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(writer.value != INVALID_HANDLE_VALUE, "native writer opens after shared asset open");
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = 8193;
    require(SetFileInformationByHandle(writer.value, FileEndOfFileInfo, &end, sizeof(end)) != 0,
            "native file grows after asset open");
    require(FlushFileBuffers(writer.value) != 0, "native file changes flush");
    FILE_BASIC_INFO changed{};
    changed.CreationTime.QuadPart = 130000000000000000LL;
    changed.LastAccessTime.QuadPart = 130000000100000000LL;
    changed.LastWriteTime.QuadPart = 130000000200000000LL;
    changed.ChangeTime.QuadPart = 130000000300000000LL;
    changed.FileAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE;
    require(SetFileInformationByHandle(writer.value, FileBasicInfo, &changed, sizeof(changed)) != 0,
            "native timestamps and attributes change after asset open");
    const auto after = files.network_information(opened.handle);
    require(after.has_value(), "changed metadata remains available");
    require_native_information(*after, writer.value);
    require(after->end_of_file == 8193 && after->end_of_file != before->end_of_file,
            "metadata refreshes length instead of caching open size");
    require(after->creation_time != before->creation_time && after->attributes != before->attributes,
            "metadata refreshes timestamps and attributes after open");
    require(after->change_time != after->last_write_time,
            "native change time remains distinct from write time");
}

void failures_map_status_without_publishing_handles() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    auto missing = files.open("game:/shader/missing.fxobj", 0);
    require(missing.status == 0xc0000034 && missing.handle == 0xffffffff && missing.size == 0,
            "missing file maps object-name-not-found and invalid handle");
    auto missing_path = files.open("game:/missing/file.fxobj", 0);
    require(missing_path.status == 0xc000003a && missing_path.handle == 0xffffffff && missing_path.size == 0,
            "missing directory maps object-path-not-found");
    const auto opened = files.open("game:/shader/Xbox360BasicShader.fxobj", 0);
    require(opened.status == 0 && opened.handle == 0x72000004, "failed requests do not consume identities");
    const auto conflict = files.open("game:/shader/Xbox360BasicShader.fxobj", 7);
    require(conflict.status == 0xc0000043 && conflict.handle == 0xffffffff && conflict.size == 0,
            "native exclusive sharing conflict is returned");
    require(files.open_count() == 1, "failed opens do not retain handles");
    files.close(opened.handle);
    const auto directory = files.open("game:/shader", 0);
    require(directory.status == 0xc00000ba && directory.handle == 0xffffffff && directory.size == 0,
            "directory is rejected as file-is-a-directory");
    require(files.open_count() == 0 && fixture.read() == fixture.bytes,
            "failures and opens preserve source file bytes");
}

void accepts_only_supported_share_bits() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    for (uint32_t share = 0; share <= 7; ++share) {
        const auto opened = files.open("game:/shader/Xbox360BasicShader.fxobj", share);
        require(opened.status == 0, "all combinations of read/write/delete sharing are accepted");
        files.close(opened.handle);
    }
    rejects([&] { files.open("game:/shader/Xbox360BasicShader.fxobj", 8); }, "unknown share bits rejected");
    require(files.open_count() == 0 && fixture.read() == fixture.bytes, "sharing does not modify files");
}

void rejects_malformed_paths_before_opening() {
    Fixture fixture;
    sfr::AssetFiles files(fixture.mount);
    std::vector<std::string> names = {
        "", "game:", "game:/", "game:shader/file", "other:/file", "C:/file", "//server/file",
        "\\\\server\\file", "/shader/file", "game://shader/file", "game:/shader//file",
        "game:/shader/", "game:/./shader/file", "game:/shader/../file", "game:/shader/..",
        "game:/shader/file:stream", "game:/shader/*.fxobj", "game:/shader/?.fxobj",
        "game:/shader/<file", "game:/shader/>file", "game:/shader/\"file", "game:/shader/|file",
        "game:/shader/file.", "game:/shader/file ", "game:/shader\t/file", "game:/shader\n/file"
    };
    names.push_back(std::string("game:/shader/fi\0le", 18));
    names.push_back("game:/shader/" + std::string(1, char(0x80)));
    names.push_back("game:/shader/" + std::string(1, char(0x7f)));
    for (const auto& name : names)
        rejects([&] { files.open(name, 0); }, "malformed guest path must be explicitly rejected");
    require(files.open_count() == 0, "malformed paths preserve host state");
    rejects([&] { files.size(0xffffffff); }, "unknown size handle rejected");
    rejects([&] { files.close(0); }, "unknown close handle rejected");
    rejects([&] { sfr::AssetFiles missing(fixture.directory / "absent"); }, "missing mount rejected");
    rejects([&] { sfr::AssetFiles file(fixture.mount / "shader" / "Xbox360BasicShader.fxobj"); },
            "regular file cannot be mounted as a root");
}

void make_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::filesystem::create_directory(link);
    NativeHandle handle{CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    require(handle.value != INVALID_HANDLE_VALUE, "junction directory open failed");
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const auto name_bytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    struct JunctionData {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        wchar_t names[2048];
    } data{};
    require(substitute.size() + 2 < 2048, "junction target too long");
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.data_length = 8 + name_bytes + 4;
    data.substitute_length = name_bytes;
    data.print_offset = name_bytes + 2;
    std::memcpy(data.names, substitute.data(), name_bytes);
    DWORD returned = 0;
    require(DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT, &data, 8 + data.data_length,
                            nullptr, 0, &returned, nullptr) != 0, "junction creation failed");
}

void rejects_reparse_escape_using_the_open_handle() {
    Fixture fixture;
    make_junction(fixture.mount / "escape", fixture.directory / "mount-escape");
    make_junction(fixture.mount / "inside", fixture.mount / "shader");
    sfr::AssetFiles files(fixture.mount);
    rejects([&] { files.open("game:/escape/outside.fxobj", 0); }, "junction outside root must be rejected");
    require(files.open_count() == 0, "escaped native handle is not retained");
    NativeHandle exclusive{CreateFileW((fixture.directory / "mount-escape" / "outside.fxobj").c_str(),
        GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(exclusive.value != INVALID_HANDLE_VALUE, "rejected escape closes its native handle");
    const auto inside = files.open("game:/inside/Xbox360BasicShader.fxobj", 0);
    require(inside.status == 0 && inside.size == fixture.bytes.size(), "reparse target within root may open");
}
}

int main() {
    try {
        opens_real_files_with_stable_identity();
        reads_native_bytes_and_preserves_independent_positions();
        reads_partial_eof_and_zero_without_unrequested_position_changes();
        rejects_invalid_read_handles_and_negative_offsets_without_advancing();
        queries_actual_native_network_information();
        queries_fresh_metadata_after_native_changes();
        failures_map_status_without_publishing_handles();
        accepts_only_supported_share_bits();
        rejects_malformed_paths_before_opening();
        rejects_reparse_escape_using_the_open_handle();
        std::cout << "Asset file checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
