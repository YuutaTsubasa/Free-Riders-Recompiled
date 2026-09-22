#include "optional_import_policy.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr uint32_t status_not_implemented = 0xc0000002u;

struct Import {
    std::string_view name;
    uint32_t address;
};

constexpr std::array unavailable_imports{
    Import{"__imp__EtxProducerRegister", 0x82acc28cu},
    Import{"__imp__EtxProducerUnregister", 0x82acc29cu},
    Import{"__imp__EtxProducerLog", 0x82acc2bcu},
};

void returns_non_success_native_status_for_exact_pairs() {
    for (const auto& import : unavailable_imports) {
        const auto status = sfr::unavailable_import_status(import.name, import.address);
        require(status == status_not_implemented, "exact unavailable import pair returns STATUS_NOT_IMPLEMENTED");
        require(*status != 0, "unavailable status is not success");
        require((*status & 0x80000000u) != 0, "unavailable status has the failure bit set");
    }
}

void rejects_cross_pairs_and_imprecise_matches() {
    for (std::size_t i = 0; i < unavailable_imports.size(); ++i) {
        const auto& import = unavailable_imports[i];
        const auto& other = unavailable_imports[(i + 1) % unavailable_imports.size()];
        require(!sfr::unavailable_import_status(import.name, other.address), "cross-paired address is rejected");
        require(!sfr::unavailable_import_status(import.name, import.address - 1), "nearby lower address is rejected");
        require(!sfr::unavailable_import_status(import.name, import.address + 1), "nearby upper address is rejected");
    }

    require(!sfr::unavailable_import_status("__imp__etxProducerRegister", 0x82acc28cu), "wrong case is rejected");
    require(!sfr::unavailable_import_status("imp__EtxProducerRegister", 0x82acc28cu), "missing prefix is rejected");
    require(!sfr::unavailable_import_status("x__imp__EtxProducerRegister", 0x82acc28cu), "extra prefix is rejected");
    require(!sfr::unavailable_import_status("__imp__EtxProducerRegisterx", 0x82acc28cu), "extra suffix is rejected");
    require(!sfr::unavailable_import_status("", 0x82acc28cu), "empty name is rejected");
}

void leaves_all_other_imports_unhandled() {
    require(!sfr::unavailable_import_status("NtAllocateVirtualMemory", 0x82acb99cu),
            "known unrelated import is unhandled");
    require(!sfr::unavailable_import_status("unknown", 0), "unknown import at zero is unhandled");
    require(!sfr::unavailable_import_status("unknown", std::numeric_limits<uint32_t>::max()),
            "unknown import at maximum address is unhandled");
    require(!sfr::unavailable_import_status("__imp__EtxProducerLog", 0), "matched name at zero is unhandled");
    require(!sfr::unavailable_import_status("__imp__EtxProducerLog", std::numeric_limits<uint32_t>::max()),
            "matched name at maximum address is unhandled");
}
}

int main() {
    try {
        returns_non_success_native_status_for_exact_pairs();
        rejects_cross_pairs_and_imprecise_matches();
        leaves_all_other_imports_unhandled();
        std::cout << "Optional import policy checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
