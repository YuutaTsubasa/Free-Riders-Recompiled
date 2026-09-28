#include "text_wrap.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
// Every character one unit wide, a Han character two, like a font.
float units(const char* first, const char* last) {
    float total = 0;
    for (const char* p = first; p < last;) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const int length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        total += length >= 3 ? 2.0f : 1.0f;
        p += length;
    }
    return total;
}
}

int main() {
    try {
        require(sfr::wrap_text("a short line", 40, units) == "a short line", "text that fits is left alone");
        require(sfr::wrap_text("one two three", 8, units) == "one two\nthree", "Latin text breaks at spaces");
        // The case that looked wrong: a Latin word, a space, then a long
        // Chinese sentence. It must not break right after the Latin word.
        const std::string wrapped = sfr::wrap_text("由實體 Kinect 追蹤玩家的身體，跟主機上一樣", 24, units);
        require(wrapped.find("Kinect\n") == std::string::npos, "a Chinese sentence does not push its start to a new line");
        require(wrapped.find('\n') != std::string::npos, "and it still wraps");
        for (size_t at = wrapped.find('\n'); at != std::string::npos; at = wrapped.find('\n', at + 1))
            require(wrapped.compare(at + 1, 3, "，") != 0 && wrapped.compare(at + 1, 3, "。") != 0,
                    "no line starts with closing punctuation");
        require(sfr::wrap_text("甲乙丙丁戊", 6, units) == "甲乙丙\n丁戊", "Han characters break between any two");
        require(sfr::wrap_text("甲乙，丙丁", 4, units) == "甲\n乙，\n丙丁", "a comma stays with the character before it");
        require(sfr::wrap_text("甲「乙丙」", 4, units) == "甲\n「乙\n丙」", "an opening bracket goes with what follows");
        require(sfr::wrap_text("line one\nline two", 40, units) == "line one\nline two", "newlines are kept");
        require(sfr::wrap_text("abcdefghij", 4, units) == "abcd\nefgh\nij", "a word longer than the line is cut");
        std::cout << "text wrap tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
