#include "voice_commands.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        require(sfr::title_word_for("ok", false) == "ok" && sfr::title_word_for("確定", false) == "ok",
                "OK in either language is the title's ok");
        require(sfr::title_word_for("start", false) == "start", "start at the title screen");
        require(sfr::title_word_for("開始", true) == "pauseopen", "start in a race opens the pause ring");
        require(sfr::title_word_for("pause", false) == "pauseopen", "pause is the title's pauseopen");
        require(sfr::title_word_for("main menu", false) == "mainmenu", "words beyond the pad's come through");
        require(sfr::title_word_for("hello", false).empty(), "anything else is nothing");
        bool has_back = false;
        for (const auto& phrase : sfr::voice_phrases()) {
            require(!sfr::title_word_for(phrase, false).empty(), "every phrase listened for means a word");
            if (phrase == "返回") has_back = true;
        }
        require(has_back, "the Chinese phrases are listened for too");
        std::string why;
        if (!sfr::VoiceRecognizer::supported())
            require(!sfr::VoiceRecognizer::open(sfr::voice_phrases(), &why) && !why.empty(),
                    "a platform without a recognizer says why");
        std::cout << "Voice command checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
