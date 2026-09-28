#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace sfr {
// Dear ImGui wraps text only at spaces. Chinese has none between its words,
// so a Chinese sentence reads to it as one long word: the line breaks early
// at the last space before it (often right after a Latin word such as
// "Kinect") and the rest is cut wherever it runs out. This breaks lines the
// way Chinese is set instead: between any two Han characters as well as at
// spaces, without starting a line with closing punctuation (，。、）」…) or
// ending one with opening punctuation (（「…). The result has a '\n' at
// each break; width(first, last) measures a run of UTF-8 text in the same
// units as max_width.
std::string wrap_text(std::string_view text, float max_width,
                      const std::function<float(const char*, const char*)>& width);
}
