#include "resource_coherency.h"
#include "guest_memory.h"
#include "physical_memory.h"
#include <array>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* detail) {
    if (!value) throw std::runtime_error(detail);
}
int main() {
    try {
        sfr::GuestMemory memory(0x10000);
        sfr::PhysicalMemory physical(memory);
        constexpr uint32_t resource = 0x10000000;
        memory.map(resource, 52);
        const std::array<uint32_t, 8> header{0x100103, 2, 0, 0, 0, 0xffff0000, 0xffff0000, 0x1000002};
        for (size_t i = 0; i < header.size(); ++i) memory.store<uint32_t>(resource + i * 4, header[i]);
        const auto address = physical.allocate(0, 0x2000, 4, 0, UINT32_MAX, 0x1000);
        memory.store<uint32_t>(resource + 32, address | 0x54);
        memory.store<uint32_t>(resource + 48, 0x54);
        memory.store<uint32_t>(address, 0x12345678);
        memory.store<uint32_t>(address + 0x1ffc, 0x87654321);
        sfr::ResourceCoherencyRequest request{0x824F3560, resource, 0, 14, 0, address, 0,
                                              address, 0x2000, 0, 0x02000000};
        unsigned waits = 0;
        const auto wait = [&] { ++waits; };
        const auto unchanged = [&] {
            for (size_t i = 0; i < header.size(); ++i)
                require(memory.load<uint32_t>(resource + i * 4) == header[i], "coherency preserves original header state");
            require(memory.load<uint32_t>(address) == 0x12345678 &&
                    memory.load<uint32_t>(address + 0x1ffc) == 0x87654321, "coherency preserves payload endpoints");
        };
        sfr::synchronize_resource_memory(memory, physical, request, wait);
        require(waits == 1, "CPU access requires actual queue completion callback");
        unchanged();
        // Every D3D lock/unlock mode is accepted (vertex buffers use other
        // callers, operations and masks than the texture unlock): the native
        // renderer reads guest memory at upload time and the caller then
        // invalidates cached copies of the range.
        auto vertex_lock = request;
        vertex_lock.caller = 0x824F2010;
        vertex_lock.operation = 10;
        vertex_lock.mask = 0x03000000;
        sfr::synchronize_resource_memory(memory, physical, vertex_lock, wait);
        require(waits == 2, "other original lock modes also wait for queue completion");
        unchanged();
        for (unsigned mode = 0; mode < 4; ++mode) {
            auto bad = request;
            switch (mode) {
            case 0: bad.size = 0; break;
            case 1: bad.size += 1; break;
            case 2: bad.resource += 1; break;
            case 3: bad.resource = 0; break;
            }
            bool stopped = false;
            try { sfr::synchronize_resource_memory(memory, physical, bad, wait); }
            catch (const sfr::RuntimeStop&) { stopped = true; }
            require(stopped && waits == 2, "invalid requests fail before queue submission");
            unchanged();
        }
        bool stopped = false;
        try {
            sfr::synchronize_resource_memory(memory, physical, request, [] {
                throw std::runtime_error("injected native queue failure");
            });
        } catch (const std::runtime_error& error) {
            stopped = std::string(error.what()) == "injected native queue failure";
        }
        require(stopped, "native failure propagates without pretending completion");
        unchanged();
        physical.mark_gpu_backed(address + 16, 16);
        stopped = false;
        try { sfr::synchronize_resource_memory(memory, physical, request, wait); }
        catch (const sfr::RuntimeStop&) { stopped = true; }
        require(stopped && waits == 2, "native mirrors cannot be reconciled by queue idleness alone");
        unchanged();
        std::cout << "Resource coherency checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
