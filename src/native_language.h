#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace sfr {
struct NativeLanguage {
    uint16_t windows_language_id;
    uint32_t xbox_language;
};

uint32_t xbox_language_from_windows(uint16_t language_id);
// POSIX hosts: the locale naming the UI language (LC_ALL, LC_MESSAGES, then
// LANG; "C" when none is set), and the LANGID it corresponds to. C/POSIX is
// US English; an unknown language, or Chinese without a region, is 0.
std::string native_locale_name();
uint16_t windows_language_from_locale(std::string_view locale);
NativeLanguage query_native_language();
}
