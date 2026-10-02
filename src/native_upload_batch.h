#pragma once

#include "plume_render_interface.h"
#include <cstdint>
#include <functional>
#include <memory>

namespace sfr {
class NativeUploadBatch {
public:
    struct Stats {
        uint64_t submissions = 0;
        double wait_ms = 0;
        uint64_t peak_bytes = 0;
        uint64_t budget_drains = 0;
    };
    NativeUploadBatch(plume::RenderDevice&, plume::RenderCommandQueue&, uint64_t budget = 32 * 1024 * 1024);
    ~NativeUploadBatch();
    NativeUploadBatch(const NativeUploadBatch&) = delete;
    NativeUploadBatch& operator=(const NativeUploadBatch&) = delete;
    // The callback records synchronously. Its staging buffer stays alive until
    // the copy fence completes; destination resources must outlive that fence.
    void record(std::unique_ptr<plume::RenderBuffer> staging, uint64_t bytes,
                const std::function<void(plume::RenderCommandList&, plume::RenderBuffer&)>& callback);
    // Submit pending copies without waiting. Slot reuse and budget pressure may
    // wait inside record(), independently of any graphics/presentation list.
    void submit();
    // Submit pending copies and retire all submitted staging resources.
    void finish();
    const Stats& stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
