#include "game_region.h"
#include "guest_memory.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void requires_region_stop(F operation, uint64_t expected_address) {
    try { operation(); }
    catch (const sfr::RuntimeStop& stop) {
        require(stop.category == "game-region" && stop.address == expected_address,
                "region rejection must distinguish invalid configuration from the unconfigured import");
        return;
    }
    throw std::runtime_error("unsupported or missing region silently produced a value");
}

void missing_configuration_remains_unknown_until_queried() {
    const sfr::GameRegion implicit;
    const sfr::GameRegion explicit_empty(std::string_view{});
    static_assert(noexcept(std::declval<const sfr::GameRegion&>().configured()));
    for (const auto* region : {&implicit, &explicit_empty}) {
        require(!region->configured(), "empty configuration must not infer a host or default region");
        for (int attempt = 0; attempt < 2; ++attempt) {
            requires_region_stop([&] { region->query(); }, 0x82ACB24C);
            require(!region->configured(), "failed queries must not install a fallback configuration");
        }
    }
}

void explicit_ntsc_us_is_stable_and_independent_of_option_storage() {
    std::string option = "--game-region=ntsc-us";
    const sfr::GameRegion region(option);
    require(region.configured(), "the exact authorized option configures a game region");
    option.assign("--game-region=pal");
    for (int attempt = 0; attempt < 3; ++attempt) {
        require(region.query() == 0x00FFu, "explicit NTSC-US always returns the exact guest region mask");
        require(region.configured(), "query does not consume the configuration");
    }
    const sfr::GameRegion temporary(std::string("--game-region=ntsc-us"));
    require(temporary.query() == 0x00FFu, "configuration does not retain a dangling string_view");
    const sfr::GameRegion separate;
    require(!separate.configured(), "one configured instance cannot configure another instance");
    requires_region_stop([&] { separate.query(); }, 0x82ACB24C);
}

void all_other_nonempty_options_fail_during_configuration() {
    for (std::string_view option : {
            "ntsc-us", "--game-region", "--game-region=", "--game-region ntsc-us",
            "--GAME-REGION=ntsc-us", "--game-region=NTSC-US", "--game-region=ntsc-Us",
            " --game-region=ntsc-us", "--game-region=ntsc-us ", "--game-region=ntsc-us\n",
            "--game-region= ntsc-us", "--game-region=ntsc-us-extra",
            "--game-region=255", "--game-region=0x00FF", "--game-region=00ff",
            "--game-region=region-free", "--game-region=all", "--game-region=pal",
            "--game-region=ntsc-j", "--game-region=unknown", "--unrelated=ntsc-us"}) {
        requires_region_stop([&] { const sfr::GameRegion invalid(option); }, 0);
    }
    // std::string_view is length-delimited: an accepted prefix followed by NUL is not exact.
    constexpr char trailing_nul[] = "--game-region=ntsc-us\0";
    constexpr char hidden_suffix[] = "--game-region=ntsc-us\0--unrelated";
    for (auto option : {std::string_view(trailing_nul, sizeof(trailing_nul) - 1),
                        std::string_view(hidden_suffix, sizeof(hidden_suffix) - 1)}) {
        requires_region_stop([&] { const sfr::GameRegion invalid(option); }, 0);
    }
}
}

int main() {
    unsigned failures = 0;
    for (auto test : {missing_configuration_remains_unknown_until_queried,
                      explicit_ntsc_us_is_stable_and_independent_of_option_storage,
                      all_other_nonempty_options_fail_during_configuration}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
