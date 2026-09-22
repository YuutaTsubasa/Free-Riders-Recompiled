#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace sfr {
std::optional<uint32_t> unavailable_import_status(std::string_view name, uint32_t address);
}
