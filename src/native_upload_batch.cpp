#include "native_upload_batch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace sfr {
struct NativeUploadBatch::Impl {
    struct Slot {
        // Destroy an unsubmitted list before the staging resources it references.
        std::vector<std::unique_ptr<plume::RenderBuffer>> staging;
        std::unique_ptr<plume::RenderCommandList> list;
        std::unique_ptr<plume::RenderCommandFence> fence;
        uint64_t bytes = 0;
        bool recording = false;
        bool in_flight = false;
    };
    plume::RenderCommandQueue& queue;
    const uint64_t budget;
    std::array<Slot, 2> slots;
    unsigned current = 0;
    uint64_t retained_bytes = 0;
    Stats stats;

    Impl(plume::RenderDevice& device, plume::RenderCommandQueue& queue, uint64_t budget)
        : queue(queue), budget(budget) {
        for (auto& slot : slots) {
            slot.list = queue.createCommandList();
            slot.fence = device.createCommandFence();
            if (!slot.list || !slot.fence)
                throw std::runtime_error("Cannot create texture upload command list or fence");
        }
    }

    void retire(Slot& slot) {
        if (!slot.in_flight) return;
        const auto start = std::chrono::steady_clock::now();
        queue.waitForCommandFence(slot.fence.get());
        stats.wait_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        slot.in_flight = false;
        retained_bytes -= slot.bytes;
        slot.bytes = 0;
        slot.staging.clear();
    }
};

NativeUploadBatch::NativeUploadBatch(plume::RenderDevice& device, plume::RenderCommandQueue& queue, uint64_t budget)
    : impl_(std::make_unique<Impl>(device, queue, budget)) {}

NativeUploadBatch::~NativeUploadBatch() {
    // Pending lists may reference textures already destroyed by the owner.
    // Only already-submitted work is safe to wait for here.
    for (auto& slot : impl_->slots) impl_->retire(slot);
}

void NativeUploadBatch::record(std::unique_ptr<plume::RenderBuffer> staging, uint64_t bytes,
    const std::function<void(plume::RenderCommandList&, plume::RenderBuffer&)>& callback) {
    if (!staging || !callback) throw std::invalid_argument("Texture upload requires staging and a callback");
    auto& impl = *impl_;
    if (impl.retained_bytes && (bytes > impl.budget || impl.retained_bytes > impl.budget - bytes)) {
        ++impl.stats.budget_drains;
        finish();
    }

    auto& slot = impl.slots[impl.current];
    impl.retire(slot);
    if (!slot.recording) {
        slot.list->begin();
        slot.recording = true;
    }
    slot.staging.push_back(std::move(staging));
    slot.bytes += bytes;
    impl.retained_bytes += bytes;
    impl.stats.peak_bytes = (std::max)(impl.stats.peak_bytes, impl.retained_bytes);
    callback(*slot.list, *slot.staging.back());
    if (bytes > impl.budget) {
        ++impl.stats.budget_drains;
        finish();
    }
}

void NativeUploadBatch::submit() {
    auto& impl = *impl_;
    auto& slot = impl.slots[impl.current];
    if (!slot.recording) return;
    slot.list->end();
    slot.recording = false;
    impl.queue.executeCommandLists(slot.list.get(), slot.fence.get());
    slot.in_flight = true;
    ++impl.stats.submissions;
    impl.current = (impl.current + 1) % impl.slots.size();
}

void NativeUploadBatch::finish() {
    submit();
    for (auto& slot : impl_->slots) impl_->retire(slot);
}

const NativeUploadBatch::Stats& NativeUploadBatch::stats() const noexcept { return impl_->stats; }
}
