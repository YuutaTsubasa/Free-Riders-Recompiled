#include "native_country.h"
#include "guest_memory.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename F> void rejects(F&& operation, const char* message) {
    try {
        operation();
    } catch (const sfr::RuntimeStop&) {
        return;
    }
    throw std::runtime_error(message);
}

struct Expected { uint32_t value; std::string_view iso; };

constexpr std::array<Expected, 107> expected{{
    Expected{1,"AE"}, {2,"AL"}, {3,"AM"}, {4,"AR"}, {5,"AT"}, {6,"AU"}, {7,"AZ"},
    {8,"BE"}, {9,"BG"}, {10,"BH"}, {11,"BN"}, {12,"BO"}, {13,"BR"}, {14,"BY"},
    {15,"BZ"}, {16,"CA"}, {18,"CH"}, {19,"CL"}, {20,"CN"}, {21,"CO"}, {22,"CR"},
    {23,"CZ"}, {24,"DE"}, {25,"DK"}, {26,"DO"}, {27,"DZ"}, {28,"EC"}, {29,"EE"},
    {30,"EG"}, {31,"ES"}, {32,"FI"}, {33,"FO"}, {34,"FR"}, {35,"GB"}, {36,"GE"},
    {37,"GR"}, {38,"GT"}, {39,"HK"}, {40,"HN"}, {41,"HR"}, {42,"HU"}, {43,"ID"},
    {44,"IE"}, {45,"IL"}, {46,"IN"}, {47,"IQ"}, {48,"IR"}, {49,"IS"}, {50,"IT"},
    {51,"JM"}, {52,"JO"}, {53,"JP"}, {54,"KE"}, {55,"KG"}, {56,"KR"}, {57,"KW"},
    {58,"KZ"}, {59,"LB"}, {60,"LI"}, {61,"LT"}, {62,"LU"}, {63,"LV"}, {64,"LY"},
    {65,"MA"}, {66,"MC"}, {67,"MK"}, {68,"MN"}, {69,"MO"}, {70,"MV"}, {71,"MX"},
    {72,"MY"}, {73,"NI"}, {74,"NL"}, {75,"NO"}, {76,"NZ"}, {77,"OM"}, {78,"PA"},
    {79,"PE"}, {80,"PH"}, {81,"PK"}, {82,"PL"}, {83,"PR"}, {84,"PT"}, {85,"PY"},
    {86,"QA"}, {87,"RO"}, {88,"RU"}, {89,"SA"}, {90,"SE"}, {91,"SG"}, {92,"SI"},
    {93,"SK"}, {95,"SV"}, {96,"SY"}, {97,"TH"}, {98,"TN"}, {99,"TR"}, {100,"TT"},
    {101,"TW"}, {102,"UA"}, {103,"US"}, {104,"UY"}, {105,"UZ"}, {106,"VE"},
    {107,"VN"}, {108,"YE"}, {109,"ZA"}
}};

void maps_the_complete_pinned_country_dataset() {
    require(expected.size() == 107, "country dataset has 107 entries and only slots 17 and 94 are holes");
    uint32_t previous = 0;
    for (const auto& entry : expected) {
        require(sfr::xbox_country_from_iso(entry.iso) == entry.value,
                "each pinned ISO code maps to its Xbox country value");
        require(entry.value > previous && entry.value != 17 && entry.value != 94,
                "dataset preserves increasing Xbox IDs and both reserved holes");
        previous = entry.value;
    }
    require(expected.front().value == 1 && expected.back().value == 109,
            "dataset covers the complete pinned numeric range");
}

void rejects_unmapped_or_malformed_geography_without_defaults() {
    const std::array<std::string_view, 13> invalid = {
        "", "U", "USA", "us", "Us", "ZZ", "17", "001", "T1", " U", "U ", "U/S",
        std::string_view("U\0S", 3)
    };
    for (const auto code : invalid)
        rejects([&] { (void)sfr::xbox_country_from_iso(code); },
                "malformed, custom, numeric, lowercase, or unmapped geography fails closed");
}

void native_query_is_an_independent_consistent_snapshot() {
#ifdef _WIN32
    wchar_t geography[16]{};
    const int length = GetUserDefaultGeoName(geography, 16);
    const bool supported_shape = length == 3 && geography[2] == L'\0' &&
                                 geography[0] >= L'A' && geography[0] <= L'Z' &&
                                 geography[1] >= L'A' && geography[1] <= L'Z';
    if (!supported_shape) {
        rejects([] { (void)sfr::query_native_country(); },
                "failed, numeric, custom, or malformed Windows geography fails closed");
        return;
    }
    const std::string actual{static_cast<char>(geography[0]), static_cast<char>(geography[1])};
    uint32_t expected_value = 0;
    for (const auto& entry : expected)
        if (entry.iso == actual) expected_value = entry.value;
    if (!expected_value) {
        rejects([] { (void)sfr::query_native_country(); },
                "unmapped Windows ISO geography fails closed without a country default");
        return;
    }
    const auto country = sfr::query_native_country();
    require(country.iso_code == actual && country.xbox_country == expected_value,
            "native query snapshots the exact Windows geography and its independent expected mapping");
#else
    require(sfr::iso_country_from_locale("zh_TW.UTF-8") == "TW" && sfr::iso_country_from_locale("C") == "US" &&
            sfr::iso_country_from_locale("de_AT@euro") == "AT" && sfr::iso_country_from_locale("en").empty(),
            "the locale's region names the country");
    std::string locale = "C";
    for (const char* name : {"LC_ALL", "LC_ADDRESS", "LANG"})
        if (const char* value = std::getenv(name); value && *value) { locale = value; break; }
    const std::string actual = sfr::iso_country_from_locale(locale);
    uint32_t expected_value = 0;
    for (const auto& entry : expected)
        if (entry.iso == actual) expected_value = entry.value;
    if (!expected_value) {
        rejects([] { (void)sfr::query_native_country(); }, "unmapped locale region fails closed");
        return;
    }
    const auto country = sfr::query_native_country();
    require(country.iso_code == actual && country.xbox_country == expected_value,
            "native query follows the POSIX locale's region");
#endif
}
}

int main() {
    try {
        maps_the_complete_pinned_country_dataset();
        rejects_unmapped_or_malformed_geography_without_defaults();
        native_query_is_an_independent_consistent_snapshot();
        std::cout << "Native country checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
