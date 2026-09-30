#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace sfr {
struct GameLanguage {
    std::string_view code;
    const char* name;
    uint32_t xbox;
};
// Languages with localized assets on the supported USA/Europe disc.
inline constexpr std::array<GameLanguage, 6> game_languages{{
    {"en", "English", 1}, {"ja", "Japanese", 2}, {"de", "Deutsch", 3},
    {"fr", "Français", 4}, {"es", "Español", 5}, {"it", "Italiano", 6}
}};
inline uint32_t game_language_id(std::string_view code) {
    for (const auto& language : game_languages)
        if (language.code == code) return language.xbox;
    return 0;
}
inline std::string_view validated_game_language(std::string_view code) {
    return game_language_id(code) ? code : "auto";
}
}
