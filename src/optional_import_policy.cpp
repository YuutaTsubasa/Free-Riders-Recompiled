#include "optional_import_policy.h"

namespace sfr {
std::optional<uint32_t> unavailable_import_status(std::string_view name, uint32_t address) {
    constexpr uint32_t status_not_implemented = 0xc0000002u;

    // These exact imports are deliberately reported as unavailable to the guest.
    if ((name == "__imp__EtxProducerRegister" && address == 0x82acc28cu) ||
        (name == "__imp__EtxProducerUnregister" && address == 0x82acc29cu) ||
        (name == "__imp__EtxProducerLog" && address == 0x82acc2bcu))
        return status_not_implemented;

    return std::nullopt;
}
}
