#include "guest_files.h"
#include "asset_files.h"
#include "guest_memory.h"

namespace sfr {
namespace {
[[noreturn]] void unsupported(uint32_t address, const char* reason) {
    throw RuntimeStop("file-open-request", address, reason);
}
}
std::string GuestFiles::object_path(uint32_t object_attributes, uint32_t& data) const {
    if (!object_attributes) unsupported(0, "file object attributes are required");
    memory_.check(object_attributes, 12);
    const uint32_t root = memory_.load<uint32_t>(object_attributes);
    const uint32_t name = memory_.load<uint32_t>(uint64_t(object_attributes)+4);
    const uint32_t attributes = memory_.load<uint32_t>(uint64_t(object_attributes)+8);
    if ((root != 0 && root != 0xfffffffd) || attributes != 0x40)
        unsupported(object_attributes, "only case-insensitive DOS mount paths are implemented");
    if (!name) unsupported(0, "counted filename is required");
    memory_.check(name, 8);
    const uint16_t length = memory_.load<uint16_t>(name);
    const uint16_t maximum = memory_.load<uint16_t>(uint64_t(name)+2);
    data = memory_.load<uint32_t>(uint64_t(name)+4);
    if (!data || !length || length > 4096 || maximum < length)
        unsupported(name, "unsupported counted filename length or pointer");
    memory_.check(data, length);
    std::string path;
    path.reserve(length);
    for (uint32_t i=0; i<length; ++i)
        path.push_back(static_cast<char>(memory_.load<uint8_t>(uint64_t(data)+i)));
    return path;
}

uint32_t GuestFiles::query_full_attributes(uint32_t object_attributes, uint32_t output) {
    uint32_t data = 0;
    const std::string path = object_path(object_attributes, data);
    last_path_ = path;
    if (!output) unsupported(0, "file attribute output is required");
    memory_.check_write(output, 56);
    AssetFiles::PathInformation result;
    try { result = files_.query_path(path); }
    catch (const std::exception& error) { throw RuntimeStop("asset-query", data, error.what()); }
    if (result.status) return result.status;  // no outputs changed
    const auto& info = result.information;
    const uint64_t fields[]{info.creation_time,info.last_access_time,info.last_write_time,
                            info.change_time,info.allocation_size,info.end_of_file};
    for (uint32_t i=0; i<6; ++i) memory_.store<uint64_t>(uint64_t(output)+i*8, fields[i]);
    memory_.store<uint32_t>(uint64_t(output)+48, info.attributes);
    memory_.store<uint32_t>(uint64_t(output)+52, 0);
    return 0;
}

uint32_t GuestFiles::open(const OpenRequest& r) {
    if (!r.handle_output || !r.io_output)
        unsupported(r.handle_output, "file handle and IO outputs must be non-null");
    memory_.check_write(r.handle_output, 4);
    memory_.check_write(r.io_output, 8);
    if (uint64_t(r.handle_output) < uint64_t(r.io_output)+8 &&
        uint64_t(r.io_output) < uint64_t(r.handle_output)+4)
        unsupported(r.handle_output, "overlapping file outputs are unsupported");
    // Read-only access (GENERIC_READ | SYNCHRONIZE, optionally with the read
    // data/attribute/EA rights) to a non-directory file. FILE_SYNCHRONOUS_IO_NONALERT
    // (0x20) selects synchronous IO; FILE_WRITE_THROUGH (0x04) and
    // FILE_SEQUENTIAL_ONLY (0x02) are hints that do not change reads.
    const bool read_access = r.access == 0x80100080 || r.access == 0x80120089;
    const bool known_options = (r.options & 0x40) && !(r.options & ~0x6Eu);
    const bool synchronous = read_access && known_options && (r.options & 0x20) && !(r.options & 0x08);
    // Without FILE_SYNCHRONOUS_IO_NONALERT reads are asynchronous, buffered
    // unless FILE_NO_INTERMEDIATE_BUFFERING is present.
    const bool asynchronous = read_access && known_options && !(r.options & 0x20);
    const bool unbuffered = r.options & 0x08;
    // FILE_ATTRIBUTE_NORMAL (0x80) is only a creation hint for an existing file.
    if ((!synchronous && !asynchronous) || r.allocation_size || (r.file_attributes && r.file_attributes != 0x80) ||
        r.share_access > 7 || r.disposition != 1)
        unsupported(r.attributes, "unsupported read-only existing-file access/options profile");
    uint32_t data = 0;
    const std::string path = object_path(r.attributes, data);
    last_path_ = path;
    AssetFiles::OpenResult result;
    try { result = files_.open(path, r.share_access, !asynchronous ? AssetFiles::OpenMode::synchronous
        : unbuffered ? AssetFiles::OpenMode::asynchronous_unbuffered : AssetFiles::OpenMode::asynchronous_buffered); }
    catch (const std::exception& error) {
        throw RuntimeStop("asset-open", data, error.what());
    }
    // All guest outputs were checked before acquiring a native handle. This
    // caller holds GuestExecution ownership, preventing concurrent guest remapping.
    memory_.store<uint32_t>(r.handle_output, result.handle);
    memory_.store<uint32_t>(r.io_output, result.status);
    memory_.store<uint32_t>(uint64_t(r.io_output)+4, result.status == 0 ? 1u : 0u);
    return result.status;
}

uint32_t GuestFiles::query_information(uint32_t handle, uint32_t io, uint32_t output,
                                      uint32_t length, uint32_t info_class) {
    if (info_class == 27) { // Xbox XFileXctdCompressionInformation, not a Windows class.
        if (length < 4) return 0xc0000004;
        if (length != 4)
            throw RuntimeStop("file-information", length, "oversized XCTD information buffers are unsupported");
        if (!files_.owns(handle)) return 0xc0000008;
        if (!output) throw RuntimeStop("file-information", output, "file information output is required");
        memory_.check_write(output, 4);
        if (io) {
            memory_.check_write(io, 8);
            if (uint64_t(io) < uint64_t(output)+4 && uint64_t(output) < uint64_t(io)+8)
                throw RuntimeStop("file-information", io, "overlapping information and IO outputs are unsupported");
        }
        // Mounted host files do not supply the Xbox XCTD metadata contract.
        // Preserve the explicit unsupported response of the pinned reference
        // (xboxkrnl_io_info.cc); the original caller decides how to handle it.
        constexpr uint32_t unavailable = 0xc000000d; // STATUS_INVALID_PARAMETER
        memory_.store<uint32_t>(output, 0);
        if (io) {
            memory_.store<uint32_t>(io, unavailable);
            memory_.store<uint32_t>(uint64_t(io)+4, 0);
        }
        return unavailable;
    }
    if (info_class != 34)
        throw RuntimeStop("file-information", info_class, "file information class is unimplemented");
    if (length < 56) return 0xc0000004; // STATUS_INFO_LENGTH_MISMATCH, no outputs changed.
    if (length != 56)
        throw RuntimeStop("file-information", length, "oversized file information buffers are unsupported");
    if (!output) throw RuntimeStop("file-information", output, "file information output is required");
    memory_.check_write(output, 56);
    if (io) {
        memory_.check_write(io, 8);
        if (uint64_t(io) < uint64_t(output)+56 && uint64_t(output) < uint64_t(io)+8)
            throw RuntimeStop("file-information", io, "overlapping information and IO outputs are unsupported");
    }
    std::optional<AssetFiles::NetworkInformation> information;
    try { information = files_.network_information(handle); }
    catch (const std::exception& error) { throw RuntimeStop("file-information", handle, error.what()); }
    if (!information) return 0xc0000008; // STATUS_INVALID_HANDLE, no outputs changed.
    const auto& info=*information;
    const uint64_t fields[]{info.creation_time,info.last_access_time,info.last_write_time,
                            info.change_time,info.allocation_size,info.end_of_file};
    for (uint32_t i=0; i<6; ++i) memory_.store<uint64_t>(uint64_t(output)+i*8, fields[i]);
    memory_.store<uint32_t>(uint64_t(output)+48, info.attributes);
    memory_.store<uint32_t>(uint64_t(output)+52, 0);
    if (io) {
        memory_.store<uint32_t>(io, 0);
        memory_.store<uint32_t>(uint64_t(io)+4, 56);
    }
    return 0;
}

uint32_t GuestFiles::set_information(uint32_t handle, uint32_t io, uint32_t input,
                                     uint32_t length, uint32_t info_class) {
    if (info_class != 14) // FilePositionInformation
        throw RuntimeStop("file-information", info_class, "file set-information class is unimplemented");
    if (length < 8) return 0xc0000004; // STATUS_INFO_LENGTH_MISMATCH
    if (!input) throw RuntimeStop("file-information", input, "file position input is required");
    memory_.check(input, 8);
    if (io) memory_.check_write(io, 8);
    uint32_t status;
    try { status = files_.set_position(handle, memory_.load<uint64_t>(input)); }
    catch (const std::exception& error) { throw RuntimeStop("file-information", handle, error.what()); }
    if (!status && io) {
        memory_.store<uint32_t>(io, 0);
        memory_.store<uint32_t>(uint64_t(io)+4, 0);
    }
    return status;
}

GuestFiles::ReadResult GuestFiles::read(const ReadRequest& r) {
    if (r.event || r.apc || r.context)
        throw RuntimeStop("file-read", r.handle, "event and APC file reads are unimplemented");
    if (r.length) {
        if (!r.buffer) throw RuntimeStop("file-read", 0, "nonempty read requires a buffer");
        memory_.check_write(r.buffer, r.length);
    }
    if (r.io_output) {
        memory_.check_write(r.io_output, 8);
        if (r.length && uint64_t(r.io_output) < uint64_t(r.buffer)+r.length &&
            uint64_t(r.buffer) < uint64_t(r.io_output)+8)
            throw RuntimeStop("file-read", r.io_output, "overlapping read buffer and IO block are unsupported");
    }
    std::optional<uint64_t> offset;
    if (r.offset_pointer) offset=memory_.load<uint64_t>(r.offset_pointer);
    AssetFiles::ReadResult result;
    try { result=files_.read(r.handle,r.length,offset); }
    catch(const std::exception& error) { throw RuntimeStop("file-read",r.handle,error.what()); }
    if (result.bytes.size()>r.length || (result.status && !result.bytes.empty()))
        throw RuntimeStop("file-read",r.handle,"invalid native transfer result");
    const auto transferred=static_cast<uint32_t>(result.bytes.size());
    // One checked store of the whole transfer (with the write-combine barrier).
    memory_.write_bytes(r.buffer,std::span<const uint8_t>(result.bytes.data(),transferred));
    if (r.io_output) {
        memory_.store<uint32_t>(r.io_output,result.status);
        memory_.store<uint32_t>(uint64_t(r.io_output)+4,transferred);
    }
    return {result.status,transferred,result.offset};
}

uint32_t GuestFiles::close(uint32_t handle) {
    try { files_.close(handle); }
    catch(const std::exception& error) { throw RuntimeStop("file-close",handle,error.what()); }
    return 0;
}
}
