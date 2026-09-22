#pragma once
#include "guest_files.h"
#include "guest_execution.h"
#include <functional>
#include <memory>

namespace sfr {
class GuestMemory;
class AssetFiles;
class NativeSyncObjects;
class GuestAsyncFiles {
public:
    using CompletionObserver = std::function<void(const GuestFiles::ReadRequest&, const GuestFiles::ReadResult&)>;
    // Native-only instrumentation; it must never access guest memory or registries.
    using SubmissionObserver = std::function<void(std::stop_token)>;
    GuestAsyncFiles(GuestMemory&, AssetFiles&, NativeSyncObjects&, GuestExecution&, CompletionObserver = {}, SubmissionObserver = {});
    ~GuestAsyncFiles();
    GuestAsyncFiles(const GuestAsyncFiles&) = delete;
    GuestAsyncFiles& operator=(const GuestAsyncFiles&) = delete;
    GuestFiles::ReadResult read(const GuestFiles::ReadRequest&, GuestExecution::Lease&);
    // Terminal shutdown: outside every execution lease; stops execution, cancels
    // and joins native work before releasing protected guest output ranges.
    void shutdown() noexcept;
    size_t pending_count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
