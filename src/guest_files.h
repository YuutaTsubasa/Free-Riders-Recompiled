#pragma once
#include <cstdint>
#include <string>

namespace sfr {
class GuestMemory;
class AssetFiles;
class GuestFiles {
public:
    struct OpenRequest {
        uint32_t handle_output, access, attributes, io_output, allocation_size;
        uint32_t file_attributes, share_access, disposition, options;
    };
    struct ReadRequest {
        uint32_t handle, event, apc, context, io_output, buffer, length, offset_pointer;
    };
    struct ReadResult { uint32_t status, transferred; uint64_t offset; };
    GuestFiles(GuestMemory& memory, AssetFiles& files) : memory_(memory), files_(files) {}
    uint32_t open(const OpenRequest& request);
    uint32_t query_information(uint32_t handle, uint32_t io_output, uint32_t output,
                               uint32_t length, uint32_t info_class);
    uint32_t set_information(uint32_t handle, uint32_t io_output, uint32_t input,
                             uint32_t length, uint32_t info_class);
    ReadResult read(const ReadRequest& request);
    uint32_t close(uint32_t handle);
    // NtQueryFullAttributesFile: FILE_NETWORK_OPEN_INFORMATION (56 bytes).
    uint32_t query_full_attributes(uint32_t object_attributes, uint32_t output);
    const std::string& last_path() const { return last_path_; }
    // The DOS path an OBJECT_ATTRIBUTES names; data receives its bytes' address.
    std::string object_path(uint32_t object_attributes, uint32_t& data) const;
private:
    GuestMemory& memory_;
    AssetFiles& files_;
    std::string last_path_;
};
}
