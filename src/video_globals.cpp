#include "video_globals.h"

namespace sfr {

VideoGlobals::VideoGlobals(GuestMemory& memory) : memory_(memory) {
    memory_.map(device_cell, 4);
}

uint32_t VideoGlobals::device_cell_for_shared_surface(uint32_t resource, uint32_t function,
                                                       uint32_t lr, uint32_t caller) const {
    if (function != shared_function || lr != shared_lr || caller != shared_caller)
        throw RuntimeStop("video-globals", function, "unsupported shared surface device-cell caller context");
    if (!resource || resource % 4)
        throw RuntimeStop("video-globals", resource, "shared surface resource must be non-null and aligned");
    memory_.check(resource, 8);
    const uint32_t header = memory_.load<uint32_t>(resource);
    const uint32_t refcount = memory_.load<uint32_t>(resource + 4);
    // Observed kinds: shared surfaces (type 4 with bit 30) and the plain
    // resources the title frees while loading (types 1..9 without bit 30:
    // buffers, textures, surfaces and their descriptors). Each destructor
    // uses the device it read only to wait for a GPU fence (+8), so a zero
    // fence means the device value is never used.
    const bool surface = (header & 0x4000000fu) == 0x40000004u;
    const uint32_t kind = header & 0x4000000fu;
    const bool plain = kind >= 1 && kind <= 9;
    if (!surface && !plain)
        throw RuntimeStop("video-globals", resource, "resource is not an observed shared surface");
    if (plain) {
        memory_.check(resource, 12);
        if (memory_.load<uint32_t>(resource + 8) != 0)
            throw RuntimeStop("video-globals", resource, "resource destruction with a pending GPU fence");
    }
    if (refcount != 0)
        throw RuntimeStop("video-globals", resource, "shared surface post-release refcount is not zero");
    return device_cell;
}

}
