#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace sfr {
// Owns read-only native files under one explicitly mounted Windows directory.
class AssetFiles {
public:
    enum class OpenMode {
        synchronous,
        asynchronous_unbuffered,
        // Overlapped reads without FILE_NO_INTERMEDIATE_BUFFERING: any offset/length.
        asynchronous_buffered,
    };

    struct OpenResult {
        uint32_t status;
        uint32_t handle;
        uint64_t size;
    };

    struct ReadResult {
        uint32_t status;
        std::vector<uint8_t> bytes;
        uint64_t offset;
    };

    struct NetworkInformation {
        uint64_t creation_time;
        uint64_t last_access_time;
        uint64_t last_write_time;
        uint64_t change_time;
        uint64_t allocation_size;
        uint64_t end_of_file;
        uint32_t attributes;
    };

    struct NativeInformation {
        uint32_t access_flags;
        uint32_t mode;
        uint32_t alignment_requirement;
    };

    enum class SubmitResult { complete, pending };
    struct AsyncReadResult {
        uint32_t status;
        uint32_t transferred;
        uint64_t offset;
        std::span<const uint8_t> bytes;
    };

    class AsyncRead {
    public:
        ~AsyncRead();
        AsyncRead(AsyncRead&&) noexcept;
        AsyncRead& operator=(AsyncRead&&) noexcept;
        AsyncRead(const AsyncRead&) = delete;
        AsyncRead& operator=(const AsyncRead&) = delete;

        SubmitResult submit();
        AsyncReadResult wait(std::stop_token stop = {});
        AsyncReadResult result() const;

    private:
        struct Impl;
        explicit AsyncRead(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
        friend class AssetFiles;
    };

    explicit AssetFiles(const std::filesystem::path& root);
    ~AssetFiles();
    AssetFiles(const AssetFiles&) = delete;
    AssetFiles& operator=(const AssetFiles&) = delete;

    OpenResult open(std::string_view guest_path, uint32_t share_access,
                    OpenMode mode = OpenMode::synchronous);
    ReadResult read(uint32_t handle, uint32_t length,
                    std::optional<uint64_t> offset = std::nullopt);
    uint64_t size(uint32_t handle) const;
    // FilePositionInformation for a synchronous handle; returns an NT status.
    uint32_t set_position(uint32_t handle, uint64_t offset);
    std::optional<NetworkInformation> network_information(uint32_t handle) const;
    // NtQueryFullAttributesFile: status 0 with the file or directory
    // information, or the NT status of the failed lookup.
    struct PathInformation { uint32_t status; NetworkInformation information; };
    PathInformation query_path(std::string_view guest_path) const;
    NativeInformation native_information(uint32_t handle) const;
    std::unique_ptr<AsyncRead> prepare_async_read(uint32_t handle, uint32_t length,
                                                  uint64_t offset);
    bool owns(uint32_t handle) const;
    void close(uint32_t handle);
    size_t open_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
