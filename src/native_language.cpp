#include "native_language.h"
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
uint32_t xbox_language_from_windows(uint16_t language_id) {
    // LANGID is a ten-bit primary language and a six-bit sublanguage.
    // Keep the pure mapping portable; values follow the Windows SDK winnt.h.
    const auto primary = language_id & 0x03FFu;
    const auto sublanguage = language_id >> 10;
    if (sublanguage != 0) {
        switch (primary) {
        case 0x09: return 1;  // English
        case 0x11: return 2;  // Japanese
        case 0x07: return 3;  // German
        case 0x0C: return 4;  // French
        case 0x0A: return 5;  // Spanish
        case 0x10: return 6;  // Italian
        case 0x12: return 7;  // Korean
        case 0x16: return 9;  // Portuguese
        case 0x15: return 11; // Polish
        case 0x19: return 12; // Russian
        case 0x04:
            switch (sublanguage) {
            case 1: // Taiwan
            case 3: // Hong Kong
            case 5: return 8; // Macau: traditional Chinese
            case 2: // China
            case 4: return 10; // Singapore: simplified Chinese
            }
            break;
        }
    }
    throw RuntimeStop("native-language", language_id,
                      "Windows UI language has no supported Xbox language mapping");
}

std::string native_locale_name() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* value = std::getenv(name); value && *value) return value;
    return "C";
}

uint16_t windows_language_from_locale(std::string_view locale) {
    // language[_REGION][.codeset][@modifier]
    locale = locale.substr(0, locale.find_first_of(".@"));
    if (locale.empty() || locale == "C" || locale == "POSIX") return 0x0409;
    const size_t split = locale.find('_');
    const std::string_view language = locale.substr(0, split);
    const std::string_view region = split == std::string_view::npos ? std::string_view{} : locale.substr(split + 1);
    static constexpr std::array<std::pair<std::string_view, uint16_t>, 11> primaries{{
        {"en", 0x09}, {"ja", 0x11}, {"de", 0x07}, {"fr", 0x0C}, {"es", 0x0A}, {"it", 0x10},
        {"ko", 0x12}, {"pt", 0x16}, {"pl", 0x15}, {"ru", 0x19}, {"zh", 0x04}}};
    for (const auto& [name, primary] : primaries) {
        if (name != language) continue;
        if (primary != 0x04) return uint16_t(1 << 10 | primary);  // the language's default region
        static constexpr std::array<std::pair<std::string_view, uint16_t>, 5> chinese{{
            {"TW", 1}, {"CN", 2}, {"HK", 3}, {"SG", 4}, {"MO", 5}}};
        for (const auto& [code, sublanguage] : chinese)
            if (code == region) return uint16_t(sublanguage << 10 | primary);
        return 0;  // the script is unknown without a region
    }
    return 0;
}

NativeLanguage query_native_language() {
#ifdef _WIN32
    const uint16_t language_id = GetUserDefaultUILanguage();
#else
    const uint16_t language_id = windows_language_from_locale(native_locale_name());
#endif
    return {language_id, xbox_language_from_windows(language_id)};
}
}
