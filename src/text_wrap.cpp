#include "text_wrap.h"

#include <vector>

namespace sfr {
namespace {
// One UTF-8 character: its code point and its length in bytes.
char32_t decode(std::string_view text, size_t at, size_t& length) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (at + length > text.size()) { length = 1; return c; }
    if (length == 1) return c;
    char32_t value = c & (0x3F >> (length - 1));
    for (size_t i = 1; i < length; ++i) value = value << 6 | (static_cast<unsigned char>(text[at + i]) & 0x3F);
    return value;
}
bool wide(char32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFFEF) || c == 0x2026 || c == 0x2014;
}
// Not at the start of a line.
bool closing(char32_t c) {
    for (const char32_t p : U"，。、；：！？）」』》〉】〕…—．,.;:!?)]}%")
        if (c == p) return true;
    return false;
}
// Not at the end of a line.
bool opening(char32_t c) {
    for (const char32_t p : U"（「『《〈【〔([{")
        if (c == p) return true;
    return false;
}
}

std::string wrap_text(std::string_view text, float max_width,
                      const std::function<float(const char*, const char*)>& width) {
    struct Character { size_t at, length; char32_t code; };
    std::vector<Character> characters;
    for (size_t at = 0; at < text.size();) {
        size_t length = 1;
        const char32_t code = decode(text, at, length);
        characters.push_back({at, length, code});
        at += length;
    }
    std::string out;
    size_t line = 0;  // index of the first character of the current line
    while (line < characters.size()) {
        // The longest run from line that fits, ending at a place a line may end.
        size_t end = line, fitted = line, fitted_break = 0;  // fitted_break: index after the last good break
        bool forced = false;
        for (; end < characters.size(); ++end) {
            if (characters[end].code == '\n') { forced = true; break; }
            const char* first = text.data() + characters[line].at;
            const char* last = text.data() + characters[end].at + characters[end].length;
            if (end > line && width(first, last) > max_width) break;
            fitted = end + 1;
            // May a line end after this character?
            const bool next_exists = end + 1 < characters.size();
            const char32_t here = characters[end].code, next = next_exists ? characters[end + 1].code : 0;
            const bool at_space = here == ' ';
            const bool between = next_exists && (wide(here) || wide(next)) && !closing(next) && !opening(here) &&
                                 next != ' ';
            if (at_space || between || !next_exists) fitted_break = end + 1;
        }
        size_t cut;
        if (forced || end == characters.size()) cut = end;
        else cut = fitted_break > line ? fitted_break : (fitted > line ? fitted : line + 1);
        // The line, without the space it may end on.
        size_t stop = cut;
        while (stop > line && characters[stop - 1].code == ' ') --stop;
        if (stop > line)
            out.append(text.substr(characters[line].at, characters[stop - 1].at + characters[stop - 1].length -
                                                            characters[line].at));
        if (cut < characters.size()) out.push_back('\n');
        line = forced ? end + 1 : cut;
        // A break at spaces leaves none at the start of the next line.
        while (!forced && line < characters.size() && characters[line].code == ' ') ++line;
    }
    return out;
}
}
