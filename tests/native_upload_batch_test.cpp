#include "native_graphics.h"
#include "native_upload_batch.h"
#include "plume_render_interface.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Copy {
    std::unique_ptr<plume::RenderBuffer> readback;
    std::vector<uint8_t> expected;
};

Copy record(sfr::NativeGraphics& graphics, sfr::NativeUploadBatch& batch,
            uint64_t bytes, uint8_t seed) {
    Copy copy{graphics.device().createBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes)),
              std::vector<uint8_t>(bytes)};
    auto staging = graphics.device().createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));
    require(staging && copy.readback, "upload and readback resources are created");
    for (size_t i = 0; i < bytes; ++i) copy.expected[i] = uint8_t(seed + i * 131u + i / 7u);
    std::memcpy(staging->map(), copy.expected.data(), bytes);
    staging->unmap();
    bool invoked = false;
    batch.record(std::move(staging), bytes, [&](plume::RenderCommandList& list, plume::RenderBuffer& source) {
        invoked = true;
        list.copyBufferRegion(copy.readback->at(0), source.at(0), bytes);
    });
    require(invoked, "record callback executes synchronously");
    return copy;
}

void verify(const std::vector<Copy>& copies) {
    for (const auto& copy : copies) {
        const void* data = copy.readback->map();
        require(data && std::memcmp(data, copy.expected.data(), copy.expected.size()) == 0,
                "GPU copies retain each upload's immutable bytes");
        copy.readback->unmap();
    }
}

void batches_many_uploads_and_reuses_slots(sfr::NativeGraphics& graphics) {
    std::vector<Copy> copies;
    sfr::NativeUploadBatch batch(graphics.device(), graphics.queue());
    batch.submit();
    batch.finish();
    require(batch.stats().submissions == 0, "empty submit and finish do not submit");
    for (unsigned pass = 0; pass < 6; ++pass) {
        for (unsigned i = 0; i < 9; ++i)
            copies.push_back(record(graphics, batch, 4096, uint8_t(pass * 31 + i)));
        require(batch.stats().submissions == pass, "nine uploads remain in one pending command list");
        batch.submit();
        require(batch.stats().submissions == pass + 1, "one submission serves nine uploads");
        batch.submit();
        require(batch.stats().submissions == pass + 1, "repeated empty submit is a no-op");
    }
    batch.finish();
    verify(copies);
    require(batch.stats().budget_drains == 0, "ordinary batches do not trigger budget drains");
    require(batch.stats().peak_bytes <= 2 * 9 * 4096, "two slots bound retained staging across reuse");
    const auto waits = batch.stats().wait_ms;
    batch.finish();
    require(batch.stats().submissions == 6 && batch.stats().wait_ms == waits,
            "second finish does not submit or wait again");
}

void drains_budget_and_oversized_uploads(sfr::NativeGraphics& graphics) {
    constexpr uint64_t budget = 3 * 4096;
    std::vector<Copy> copies;
    sfr::NativeUploadBatch batch(graphics.device(), graphics.queue(), budget);
    for (unsigned i = 0; i < 10; ++i)
        copies.push_back(record(graphics, batch, 4096, uint8_t(i + 71)));
    batch.finish();
    verify(copies);
    require(batch.stats().submissions == 4, "budget groups ten uploads into four submissions");
    require(batch.stats().budget_drains == 3, "each exhausted staging budget drains uploads");
    require(batch.stats().peak_bytes == budget, "normal uploads never retain more than the budget");

    copies.push_back(record(graphics, batch, 4096, 111));
    copies.push_back(record(graphics, batch, budget * 2, 143));
    require(batch.stats().submissions == 6, "oversized upload immediately submits after draining earlier copies");
    require(batch.stats().peak_bytes == budget * 2, "one oversized staging allocation is allowed");
    require(batch.stats().budget_drains == 5, "oversized upload drains preceding and oversized batches");
    verify(copies); // Oversized record must complete before returning.
    batch.finish();
    require(batch.stats().submissions == 6, "finish after oversized upload has nothing to submit");
}

void destruction_waits_submitted_and_discards_pending(sfr::NativeGraphics& graphics) {
    std::vector<Copy> copies;
    {
        sfr::NativeUploadBatch batch(graphics.device(), graphics.queue());
        copies.push_back(record(graphics, batch, 4096, 23));
        batch.submit();
    }
    verify(copies);
    // Initialize a destination, then record (but do not submit) an overwrite.
    auto& destination = copies.front();
    {
        sfr::NativeUploadBatch batch(graphics.device(), graphics.queue());
        auto staging = graphics.device().createBuffer(plume::RenderBufferDesc::UploadBuffer(4096));
        std::memset(staging->map(), 0, 4096);
        staging->unmap();
        batch.record(std::move(staging), 4096, [&](plume::RenderCommandList& list, plume::RenderBuffer& source) {
            list.copyBufferRegion(destination.readback->at(0), source.at(0), 4096);
        });
    }
    graphics.wait_idle();
    verify(copies);
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 1) {
#ifdef _WIN32
            _putenv_s("SFR_GRAPHICS", argv[1]);
#else
            setenv("SFR_GRAPHICS", argv[1], 1);
#endif
        }
        sfr::NativeGraphics graphics;
        graphics.initialize();
        if (argc > 1) require((std::string(argv[1]) == "vulkan") == (graphics.backend() == sfr::GraphicsBackend::vulkan),
                              "requested GPU backend must be available");
        batches_many_uploads_and_reuses_slots(graphics);
        drains_budget_and_oversized_uploads(graphics);
        destruction_waits_submitted_and_discards_pending(graphics);
        std::cout << "Native upload batch GPU checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
