#pragma once

#include "guest_memory.h"

namespace sfr {

class VideoGlobals {
public:
    static constexpr uint32_t device_cell = 0x72500000;
    static constexpr uint32_t shared_function = 0x824f19e8;
    static constexpr uint32_t shared_lr = 0x824f1a04;
    static constexpr uint32_t shared_caller = 0x824f1fb8;

    explicit VideoGlobals(GuestMemory& memory);
    uint32_t device_cell_for_shared_surface(uint32_t resource, uint32_t function,
                                            uint32_t lr, uint32_t caller) const;

private:
    GuestMemory& memory_;
};

}
