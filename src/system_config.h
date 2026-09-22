#pragma once
#include <cstdint>
#include <optional>

namespace sfr {
class GuestMemory;
class SystemConfig {
public:
    SystemConfig(GuestMemory& memory, uint32_t language, std::optional<uint32_t> country = {});
    uint32_t query(uint16_t category, uint16_t setting, uint32_t buffer,
                   uint16_t capacity, uint32_t required) const;
    uint32_t language() const noexcept { return language_; }
    std::optional<uint32_t> country() const noexcept { return country_; }
private:
    GuestMemory& memory_;
    uint32_t language_;
    std::optional<uint32_t> country_;
};
}
