#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sfr {
struct NativeCountry {
    std::string iso_code;
    uint32_t xbox_country;
};

uint32_t xbox_country_from_iso(std::string_view iso_code);
// POSIX hosts: the region of a locale name ("zh_TW.UTF-8" is TW; C and
// POSIX are US), or empty when it names none.
std::string iso_country_from_locale(std::string_view locale);
NativeCountry query_native_country();
}
