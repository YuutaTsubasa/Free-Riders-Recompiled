#include "voice_commands.h"

#include <utility>

namespace sfr {
namespace {
// Phrase, title word. Several phrases may say the same word; the title's
// vocabulary around 0x821A84D8 also has "restart", "replay" and "mainmenu".
const std::vector<std::pair<std::string, std::string>>& table() {
    static const std::vector<std::pair<std::string, std::string>> phrases{
        {"start", "start"},       {"開始", "start"},
        {"ok", "ok"},             {"okay", "ok"},           {"好", "ok"},          {"確定", "ok"},
        {"back", "back"},         {"返回", "back"},         {"上一頁", "back"},
        {"next", "next"},         {"下一個", "next"},       {"下一步", "next"},
        {"up", "up"},             {"往上", "up"},
        {"down", "down"},         {"往下", "down"},
        {"left", "left"},         {"往左", "left"},
        {"right", "right"},       {"往右", "right"},
        {"pause", "pauseopen"},   {"暫停", "pauseopen"},
        {"restart", "restart"},   {"重新開始", "restart"},   {"重來", "restart"},
        {"replay", "replay"},     {"重播", "replay"},
        {"main menu", "mainmenu"}, {"主選單", "mainmenu"},
    };
    return phrases;
}
}

const std::vector<std::string>& voice_phrases() {
    static const std::vector<std::string> phrases = [] {
        std::vector<std::string> all;
        for (const auto& [phrase, word] : table()) all.push_back(phrase);
        return all;
    }();
    return phrases;
}

std::string_view title_word_for(std::string_view phrase, bool racing) {
    for (const auto& [known, word] : table())
        if (known == phrase) return word == "start" && racing ? std::string_view("pauseopen") : std::string_view(word);
    return {};
}

VoiceRecognizer::~VoiceRecognizer() = default;

}
