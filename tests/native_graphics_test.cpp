#include "native_graphics.h"

#ifdef _WIN32
#include "plume_d3d12.h"
#endif
#include "plume_render_interface.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Operation>
bool rejects_with(Operation operation) {
    try {
        operation();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

void starts_uninitialized_and_guards_access() {
    sfr::NativeGraphics graphics;

    require(!graphics.initialized(), "native graphics starts uninitialized");
    require(rejects_with<std::logic_error>([&] { (void)graphics.device(); }),
            "device access before initialization throws logic_error");
    require(rejects_with<std::logic_error>([&] { (void)graphics.queue(); }),
            "queue access before initialization throws logic_error");
    require(rejects_with<std::logic_error>([&] { graphics.wait_idle(); }),
            "queue idle wait before initialization throws logic_error");
}

void copies_repeatedly_on_the_real_gpu_and_initializes_idempotently() {
    sfr::NativeGraphics graphics;
    graphics.initialize();
    require(graphics.initialized(), "native graphics reports initialized");
#ifdef _WIN32
    require(sfr::selected_graphics_backend() != sfr::GraphicsBackend::d3d12 ||
            static_cast<plume::D3D12CommandQueue*>(&graphics.queue())->d3d != nullptr,
            "initialized graphics owns a native D3D12 command queue");
#endif

    unsigned cancellation_checks = 0;
    require(rejects_with<std::runtime_error>([&] {
                graphics.wait_idle([&] { ++cancellation_checks; return true; });
            }) && cancellation_checks == 1,
            "pre-cancelled idle wait stops before queue synchronization");
    cancellation_checks = 0;
    require(rejects_with<std::runtime_error>([&] {
                graphics.wait_idle([&] { return ++cancellation_checks == 2; });
            }) && cancellation_checks == 2,
            "cancellation after signaling is not reported as completion");

    plume::RenderDevice* const first_device = &graphics.device();
    plume::RenderCommandQueue* const first_queue = &graphics.queue();
    graphics.initialize();
    require(&graphics.device() == first_device, "idempotent initialization preserves device identity");
    require(&graphics.queue() == first_queue, "idempotent initialization preserves queue identity");

    constexpr std::size_t byte_count = 4096;
    auto upload = graphics.device().createBuffer(plume::RenderBufferDesc::UploadBuffer(byte_count));
    auto readback = graphics.device().createBuffer(plume::RenderBufferDesc::ReadbackBuffer(byte_count));
    auto command_list = graphics.queue().createCommandList();
    require(upload != nullptr, "real upload buffer was created");
    require(readback != nullptr, "real readback buffer was created");
    require(command_list != nullptr, "real direct command list was created");

    std::array<std::byte, byte_count> expected{};
    for (uint32_t pass = 0; pass < 4; ++pass) {
        for (std::size_t i = 0; i < expected.size(); ++i)
            expected[i] = std::byte((i * 131u + i / 7u + pass * 83u) & 0xffu);

        void* const upload_data = upload->map();
        require(upload_data != nullptr, "upload buffer maps");
        std::memcpy(upload_data, expected.data(), expected.size());
        upload->unmap();

        command_list->begin();
        command_list->copyBufferRegion(readback->at(0), upload->at(0), expected.size());
        command_list->end();
        graphics.queue().executeCommandLists(command_list.get());
        graphics.wait_idle();

        const void* const readback_data = readback->map();
        require(readback_data != nullptr, "readback buffer maps after GPU completion");
        require(std::memcmp(readback_data, expected.data(), expected.size()) == 0,
                "wait_idle completes each repeated GPU copy with fresh contents");
        readback->unmap();
    }
}
}

int main() {
    try {
        starts_uninitialized_and_guards_access();
        copies_repeatedly_on_the_real_gpu_and_initializes_idempotently();
        std::cout << "Native graphics checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
