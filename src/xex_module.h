#pragma once
#include "guest_memory.h"
#include <span>
#include <vector>

namespace sfr {
class XexModule {
public:
    static constexpr uint32_t header_address = 0x71010000;
    static constexpr uint32_t handle_address = 0x71000000;
    static constexpr uint32_t module_address = 0x71000040;
    XexModule(GuestMemory& memory, std::span<const uint8_t> header);
    uint32_t header_field(uint32_t queried_header, uint32_t key) const;
    uint32_t check_privilege(uint32_t privilege) const;
    uint32_t get_module_handle(uint32_t name, uint32_t output) const;
    uint32_t get_module_section(uint32_t handle, uint32_t name, uint32_t data_output,
                                uint32_t size_output) const;
private:
    GuestMemory& memory_;
    // Lookup results are validated from the supplied header once, so guest
    // writes to its mapped bytes cannot introduce unchecked offset pointers.
    struct Field { uint32_t key, result; };
    std::vector<Field> fields_;
    struct Resource { std::string name; uint32_t address, size; };
    std::vector<Resource> resources_;
};
}
