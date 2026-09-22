#include "native_language.h"
#include "guest_memory.h"
#include <array>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<typename F> static void rejects(F operation, uint16_t language_id) {
    try { operation(); }
    catch (const sfr::RuntimeStop& stop) {
        require(stop.category == "native-language" && stop.address == language_id,
                "language rejection identifies the actual unsupported LANGID");
        return;
    }
    throw std::runtime_error("unsupported language silently mapped to a guest language");
}

struct LanguageCase { uint16_t windows; uint32_t xbox; };
static constexpr std::array<LanguageCase, 26> regional_cases{{
    {0x0409, 1}, {0x0809, 1}, {0x0C09, 1}, {0x4009, 1},
    {0x0411, 2},
    {0x0407, 3}, {0x0807, 3}, {0x0C07, 3}, {0x1407, 3},
    {0x040C, 4}, {0x080C, 4}, {0x0C0C, 4}, {0x140C, 4},
    {0x040A, 5}, {0x080A, 5}, {0x0C0A, 5}, {0x540A, 5},
    {0x0410, 6}, {0x0810, 6},
    {0x0412, 7},
    {0x0416, 9}, {0x0816, 9},
    {0x0415, 11}, {0x0419, 12},
    {0x1809, 1}, {0x180C, 4}
}};

static void regional_languages_preserve_guest_enum_values() {
    for (const auto test : regional_cases)
        require(sfr::xbox_language_from_windows(test.windows) == test.xbox,
                "regional Windows LANGID maps to the exact Xbox language enum");
}

static void chinese_script_is_selected_from_explicit_sublanguage() {
    for (const auto test : std::array<LanguageCase, 5>{{
             {0x0404, 8}, {0x0C04, 8}, {0x1404, 8}, {0x0804, 10}, {0x1004, 10}}})
        require(sfr::xbox_language_from_windows(test.windows) == test.xbox,
                "Chinese Taiwan/Hong Kong/Macau and China/Singapore retain their script");
    for (uint16_t sublanguage = 0; sublanguage < 64; ++sublanguage) {
        if (sublanguage >= 1 && sublanguage <= 5) continue;
        const auto language_id = static_cast<uint16_t>((sublanguage << 10) | 4);
        rejects([&] { sfr::xbox_language_from_windows(language_id); }, language_id);
    }
}

static void unknown_neutral_and_custom_languages_do_not_guess_english() {
    for (uint16_t language_id : {0x0000, 0x0400, 0x0800, 0x0C00, 0x1000, 0x1400,
                                 0x007F, 0x047F, 0xFFFF, 0x0401, 0x0413, 0x041D,
                                 0x041F, 0x0439, 0x0422})
        rejects([&] { sfr::xbox_language_from_windows(language_id); }, language_id);
    for (uint16_t primary : {0x09, 0x11, 0x07, 0x0C, 0x0A, 0x10, 0x12, 0x16, 0x15, 0x19})
        rejects([&] { sfr::xbox_language_from_windows(primary); }, primary);
}

static void native_query_uses_actual_user_ui_language() {
#ifdef _WIN32
    const auto actual = GetUserDefaultUILanguage();
    uint32_t expected = 0;
    // Independent test oracle: SDK language constants and explicit Xbox values.
    if (SUBLANGID(actual) != SUBLANG_NEUTRAL) {
        switch (PRIMARYLANGID(actual)) {
        case LANG_ENGLISH: expected = 1; break;
        case LANG_JAPANESE: expected = 2; break;
        case LANG_GERMAN: expected = 3; break;
        case LANG_FRENCH: expected = 4; break;
        case LANG_SPANISH: expected = 5; break;
        case LANG_ITALIAN: expected = 6; break;
        case LANG_KOREAN: expected = 7; break;
        case LANG_PORTUGUESE: expected = 9; break;
        case LANG_POLISH: expected = 11; break;
        case LANG_RUSSIAN: expected = 12; break;
        case LANG_CHINESE:
            switch (SUBLANGID(actual)) {
            case SUBLANG_CHINESE_TRADITIONAL:
            case SUBLANG_CHINESE_HONGKONG:
            case SUBLANG_CHINESE_MACAU: expected = 8; break;
            case SUBLANG_CHINESE_SIMPLIFIED:
            case SUBLANG_CHINESE_SINGAPORE: expected = 10; break;
            }
            break;
        }
    }
    if (!expected) {
        rejects([] { sfr::query_native_language(); }, actual);
        return;
    }
    const auto result = sfr::query_native_language();
    require(result.windows_language_id == actual && result.xbox_language == expected,
            "native query snapshots actual Windows UI LANGID and its guest mapping");
#else
    const uint16_t actual = sfr::windows_language_from_locale(sfr::native_locale_name());
    if (!actual) {
        rejects([] { sfr::query_native_language(); }, 0);
        return;
    }
    const auto result = sfr::query_native_language();
    require(result.windows_language_id == actual && result.xbox_language == sfr::xbox_language_from_windows(actual),
            "native query follows the POSIX locale");
    require(sfr::windows_language_from_locale("C") == 0x0409 && sfr::windows_language_from_locale("POSIX") == 0x0409,
            "the C locale is US English");
    require(sfr::windows_language_from_locale("zh_TW.UTF-8") == 0x0404 &&
            sfr::windows_language_from_locale("zh_CN.UTF-8") == 0x0804 &&
            sfr::windows_language_from_locale("zh_HK") == 0x0C04,
            "Chinese locales keep their script from the region");
    require(sfr::windows_language_from_locale("ja_JP.UTF-8") == 0x0411 &&
            sfr::windows_language_from_locale("de_AT@euro") == 0x0407,
            "other languages use their default sublanguage");
    require(!sfr::windows_language_from_locale("zh") && !sfr::windows_language_from_locale("nl_NL.UTF-8"),
            "unknown script or language is not guessed");
#endif
}

int main() {
    unsigned failures = 0;
    for (auto test : {regional_languages_preserve_guest_enum_values,
                      chinese_script_is_selected_from_explicit_sublanguage,
                      unknown_neutral_and_custom_languages_do_not_guess_english,
                      native_query_uses_actual_user_ui_language}) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    }
    if (failures) return 1;
    std::cout << "Native language checks passed\n";
    return 0;
}
