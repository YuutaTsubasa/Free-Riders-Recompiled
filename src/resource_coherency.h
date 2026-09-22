#pragma once
#include <cstdint>
#include <functional>

namespace sfr {
class GuestMemory;
class PhysicalMemory;
struct ResourceCoherencyRequest {
    uint32_t caller, resource;
    uint64_t token;
    uint32_t operation, secondary, base, secondary_base, address, size, flags, mask;
};
// CPU-only physical backing has no native texture cache to reconcile. A future
// GPU representation must mark its allocation GPU-backed before publication.
void synchronize_resource_memory(GuestMemory&, PhysicalMemory&,
                                 const ResourceCoherencyRequest&,
                                 const std::function<void()>& wait_for_queue);
}
