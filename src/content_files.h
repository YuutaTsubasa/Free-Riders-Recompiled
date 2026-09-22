#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sfr {
class GuestMemory;

// Title content (saves) on the one storage device of this runtime. The title
// creates a content package with XamContentCreateEx, which mounts it as a
// root ("sav:"), and then reads and writes ordinary files under that root.
// A package is a host directory:
//   <root>/<xuid as 16 hex digits>/<content type as 8 hex digits>/<file name>/
// with the display name beside it in <file name>.name (UTF-16LE).
// File handles are 0x72300004..0x723FFFFC.
class ContentFiles {
public:
    static constexpr uint32_t device_id = 1;  // the hard drive
    static constexpr uint32_t first_handle = 0x72300004u, handle_limit = 0x72400000u;
    static bool is_handle_range(uint32_t handle) { return handle >= 0x72300000u && handle < handle_limit; }

    ContentFiles(GuestMemory& memory, std::filesystem::path root);
    ~ContentFiles();

    // XamContentCreateEx: content_data is an XCONTENT_DATA (0x134 bytes).
    // Returns a Win32 error; *disposition_output is 1 (created) or 2 (opened).
    uint32_t create(uint64_t xuid, uint32_t root_name, uint32_t content_data, uint32_t flags,
                    uint32_t disposition_output, uint32_t license_output);
    // XamContentClose: unmounts the root.
    uint32_t close(uint32_t root_name);
    // XamContentGetCreator: this runtime's content is always the profile's own.
    uint32_t creator(uint64_t xuid, uint32_t content_data, uint32_t is_creator_output, uint32_t xuid_output);
    // XamContentGetDeviceData: XDEVICE_DATA (0x50 bytes).
    uint32_t device_data(uint32_t device, uint32_t output);
    // Existing packages of this type, for XamContentCreateEnumerator.
    struct Package { uint32_t content_type; std::u16string display_name; std::string file_name; };
    std::vector<Package> packages(uint64_t xuid, uint32_t content_type) const;
    static void write_content_data(GuestMemory& memory, uint32_t address, const Package& package);

    // True when path ("root:\name", optionally "\??\"-prefixed) is under a mount.
    bool claims(const std::string& path) const;
    // NtCreateFile/NtOpenFile on a claimed path; returns an NT status and
    // writes the handle and IO_STATUS_BLOCK like the kernel.
    uint32_t open(const std::string& path, uint32_t handle_output, uint32_t access, uint32_t io_output,
                  uint32_t share_access, uint32_t disposition, uint32_t options);
    bool owns(uint32_t handle) const;
    uint32_t read(uint32_t handle, uint32_t io_output, uint32_t buffer, uint32_t length, uint32_t offset_pointer);
    uint32_t write(uint32_t handle, uint32_t io_output, uint32_t buffer, uint32_t length, uint32_t offset_pointer);
    uint32_t query_information(uint32_t handle, uint32_t io_output, uint32_t output, uint32_t length, uint32_t info_class);
    uint32_t set_information(uint32_t handle, uint32_t io_output, uint32_t input, uint32_t length, uint32_t info_class);
    uint32_t query_full_attributes(const std::string& path, uint32_t output);
    uint32_t flush(uint32_t handle, uint32_t io_output);
    uint32_t close_handle(uint32_t handle);
    std::filesystem::path host_path(const std::string& path) const;  // for traces

private:
    struct File;
    struct Mount { std::filesystem::path directory; };
    std::filesystem::path package_directory(uint64_t xuid, uint32_t content_type, const std::string& file_name) const;
    std::string read_root(uint32_t root_name) const;
    File* file(uint32_t handle) const;
    void complete(uint32_t io_output, uint32_t status, uint32_t information);

    GuestMemory& memory_;
    std::filesystem::path root_;
    std::map<std::string, Mount> mounts_;  // lower-case root name
    std::map<uint32_t, std::unique_ptr<File>> files_;
    uint32_t next_handle_ = first_handle;
};
}
