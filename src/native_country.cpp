#include "native_country.h"

#include "guest_memory.h"

#include <array>
#include <cstdlib>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sfr {
namespace {
// Xbox IDs from Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// xboxkrnl_xconfig.cc. Slots 0, 17 and 94 have no supported country.
constexpr std::array<std::string_view, 110> countries = {
    "", "AE", "AL", "AM", "AR", "AT", "AU", "AZ", "BE", "BG", "BH", "BN", "BO", "BR",
    "BY", "BZ", "CA", "", "CH", "CL", "CN", "CO", "CR", "CZ", "DE", "DK", "DO", "DZ",
    "EC", "EE", "EG", "ES", "FI", "FO", "FR", "GB", "GE", "GR", "GT", "HK", "HN", "HR",
    "HU", "ID", "IE", "IL", "IN", "IQ", "IR", "IS", "IT", "JM", "JO", "JP", "KE", "KG",
    "KR", "KW", "KZ", "LB", "LI", "LT", "LU", "LV", "LY", "MA", "MC", "MK", "MN", "MO",
    "MV", "MX", "MY", "NI", "NL", "NO", "NZ", "OM", "PA", "PE", "PH", "PK", "PL", "PR",
    "PT", "PY", "QA", "RO", "RU", "SA", "SE", "SG", "SI", "SK", "", "SV", "SY", "TH",
    "TN", "TR", "TT", "TW", "UA", "US", "UY", "UZ", "VE", "VN", "YE", "ZA"
};
}

uint32_t xbox_country_from_iso(std::string_view iso_code) {
    if (iso_code.size() != 2 || iso_code[0] < 'A' || iso_code[0] > 'Z' ||
        iso_code[1] < 'A' || iso_code[1] > 'Z')
        throw RuntimeStop("native-country", 0,
                          "native geography requires an uppercase two-letter ISO code");
    for (uint32_t value = 1; value < countries.size(); ++value)
        if (countries[value] == iso_code) return value;
    throw RuntimeStop("native-country", 0,
                      "native ISO geography has no supported Xbox country mapping");
}

std::string iso_country_from_locale(std::string_view locale) {
    locale = locale.substr(0, locale.find_first_of(".@"));
    if (locale.empty() || locale == "C" || locale == "POSIX") return "US";
    const size_t split = locale.find('_');
    return split == std::string_view::npos ? std::string{} : std::string(locale.substr(split + 1));
}

NativeCountry query_native_country() {
#ifdef _WIN32
    // ISO alpha-2 and numeric M49 names both fit, including their terminator.
    wchar_t geography[16]{};
    const int length = GetUserDefaultGeoName(geography, 16);
    if (length == 0)
        throw RuntimeStop("native-country", GetLastError(),
                          "GetUserDefaultGeoName failed");
    if (length != 3 || geography[2] != L'\0' || geography[0] < L'A' ||
        geography[0] > L'Z' || geography[1] < L'A' || geography[1] > L'Z')
        throw RuntimeStop("native-country", 0,
                          "Windows geography is not an uppercase two-letter ISO code");
    std::string iso_code{static_cast<char>(geography[0]), static_cast<char>(geography[1])};
    const uint32_t xbox_country = xbox_country_from_iso(iso_code);
    return {std::move(iso_code), xbox_country};
#else
    // The formats locale names the region (LC_ALL, LC_ADDRESS, then LANG).
    std::string locale = "C";
    for (const char* name : {"LC_ALL", "LC_ADDRESS", "LANG"})
        if (const char* value = std::getenv(name); value && *value) { locale = value; break; }
    std::string iso_code = iso_country_from_locale(locale);
    const uint32_t xbox_country = xbox_country_from_iso(iso_code);
    return {std::move(iso_code), xbox_country};
#endif
}
}
