#include "resource_coherency.h"
#include "guest_memory.h"
#include "physical_memory.h"

namespace sfr {
void synchronize_resource_memory(GuestMemory& memory, PhysicalMemory& physical,
                                 const ResourceCoherencyRequest& request,
                                 const std::function<void()>& wait_for_queue) {
    const auto reject = [&](const char* detail) {
        throw RuntimeStop("resource-coherency", request.address, detail);
    };
    // The native renderer reads guest memory when it uploads a resource, so
    // CPU access needs no cache maintenance, only completion of queued GPU
    // work; cached native copies are invalidated by the caller.
    if (!request.resource || (request.resource & 3)) reject("invalid original resource header");
    memory.check(request.resource, 24);  // D3DResource common header (vertex buffers are 32 bytes)
    if (!request.size) reject("empty resource access range");
    physical.require_cpu_only_range(request.address, request.size);
    if (!wait_for_queue) reject("native queue completion is unavailable");
    wait_for_queue();
}
}
