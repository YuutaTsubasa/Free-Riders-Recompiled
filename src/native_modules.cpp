#include "native_modules.h"

#include <string>

namespace sfr {
std::optional<uint32_t> module_status_to_dos_error(uint32_t status) {
    if (status == 0xc0000263) return 127;
    return std::nullopt;
}

namespace {
bool is_missing_optional_xam_export(uint32_t ordinal) {
    switch (ordinal) {
    case 0xaff:
    case 0xb00:
    case 0xb0b:
    case 0xb10:
    case 0x305:
    case 0x30b:
    // Original 8270CDF8 explicitly falls back to its plain startup import
    // when this optional Ex export is absent. Its SDK extension is not hosted.
    case 0x24:
        return true;
    default:
        return false;
    }
}

std::string checked_module_name(GuestMemory& memory, uint32_t address) {
    if (!address)
        throw RuntimeStop("native-module-request", address, "module name must be non-null");

    std::string name;
    name.reserve(259);
    for (uint64_t offset = 0; offset < 260; ++offset) {
        const uint64_t current = uint64_t(address) + offset;
        if (current >= GuestMemory::address_space_size)
            throw RuntimeStop("native-module-request", address, "module name crosses guest address space");
        uint8_t byte = 0;
        try {
            byte = memory.load<uint8_t>(current);
        } catch (const RuntimeStop&) {
            throw RuntimeStop("native-module-request", address, "module name is not a bounded guest C string");
        }
        if (!byte) return name;
        name.push_back(static_cast<char>(byte));
    }
    throw RuntimeStop("native-module-request", address, "module name exceeds 259 bytes");
}
}

NativeModules::NativeModules(GuestMemory& memory) : memory_(memory) {
    memory_.map(xam_handle, 4);
    memory_.add_import_variable(xam_handle, "native XAM namespace handle");
}

uint32_t NativeModules::load_image(uint32_t name_address, uint32_t flags,
                                   uint32_t min_version, uint32_t output) {
    const auto name = checked_module_name(memory_, name_address);
    if (name != "xam.xex" || flags != 9 || min_version != 0)
        throw RuntimeStop("native-module-request", name_address,
                          "only xam.xex with flags 9 and minimum version 0 is supported");

    if (!output)
        throw RuntimeStop("memory-access", output, "module handle output must be non-null");
    memory_.check_write(output, 4);
    if (xam_load_count_ == 65535)
        throw RuntimeStop("native-module-count", xam_handle, "native XAM load count limit reached");

    memory_.store<uint32_t>(output, xam_handle);
    ++xam_load_count_;
    return 0;
}

uint32_t NativeModules::get_module_handle(uint32_t name_address, uint32_t output) {
    const auto name = checked_module_name(memory_, name_address);
    if (name != "xam.xex")
        throw RuntimeStop("native-module-request", name_address, "unknown native module namespace");
    if (!output)
        throw RuntimeStop("memory-access", output, "module handle output must be non-null");
    memory_.store<uint32_t>(output, xam_handle);
    // XAM is resident in this runtime. A query publishes its borrowed identity
    // only after the complete output write succeeds; it acquires no reference.
    xam_queried_ = true;
    return 0;
}

uint32_t NativeModules::get_procedure_address(uint32_t handle, uint32_t ordinal, uint32_t output) {
    if (handle != xam_handle || (xam_load_count_ == 0 && !xam_queried_) || !is_missing_optional_xam_export(ordinal))
        throw RuntimeStop("native-module-export", ordinal,
                          "only queried or acquired XAM known optional exports are recognized");
    if (!output)
        throw RuntimeStop("memory-access", output, "procedure output must be non-null");
    memory_.check_write(output, 4);
    memory_.store<uint32_t>(output, 0);
    return 0xc0000263;
}

uint32_t NativeModules::unload_image(uint32_t handle) {
    if (handle != xam_handle || xam_load_count_ == 0)
        throw RuntimeStop("native-module-request", handle, "only acquired XAM can be unloaded");
    return 0;
}
}
