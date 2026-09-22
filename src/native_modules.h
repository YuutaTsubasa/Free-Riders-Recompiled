#pragma once

#include "guest_memory.h"

#include <cstdint>
#include <optional>

namespace sfr {
std::optional<uint32_t> module_status_to_dos_error(uint32_t status);

class NativeModules {
public:
    static constexpr uint32_t xam_handle = 0x71500000;
    // Target ABI from both verified game import-library version/minimum fields.
    // This does not imply every export is implemented; unknown exports trap.
    static constexpr uint32_t compatibility_version = 0x20308000; // 2.0.12416.0

    explicit NativeModules(GuestMemory& memory);
    uint32_t get_module_handle(uint32_t name, uint32_t output);
    uint32_t load_image(uint32_t name, uint32_t flags, uint32_t min_version, uint32_t output);
    uint32_t get_procedure_address(uint32_t handle, uint32_t ordinal, uint32_t output);
    uint32_t unload_image(uint32_t handle);
    uint32_t xam_load_count() const { return xam_load_count_; }

private:
    GuestMemory& memory_;
    uint32_t xam_load_count_ = 0;
    bool xam_queried_ = false;
};
}
