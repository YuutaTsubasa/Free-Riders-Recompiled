#include "voice_language.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        using sfr::voice_language_path;
        // English voices for a Japanese game, and the other way round.
        require(voice_language_path("game:\\sound\\SRN_Jact_SN.csb", "en") == "game:\\sound\\SRN_Eact_SN.csb",
                "a Japanese act sheet opens the English one");
        require(voice_language_path("game:\\sound\\SRN_stream_voice_j.cpk", "en") ==
                    "game:\\sound\\SRN_stream_voice_e.cpk",
                "the Japanese stream opens the English one");
        require(voice_language_path("game:\\sound\\SRN_stream_voice_j.csb", "en") ==
                    "game:\\sound\\SRN_stream_voice_e.csb",
                "and its cue sheet");
        require(voice_language_path("game:/sound/srn_eact_am.csb", "ja") == "game:/sound/srn_jact_am.csb",
                "lower case and forward slashes keep their case");
        require(voice_language_path("game:\\sound\\SRN_stream_voice_e.cpk", "ja") ==
                    "game:\\sound\\SRN_stream_voice_j.cpk",
                "an English stream opens the Japanese one");
        // Nothing to do: already the wanted set, not a voice file, or no choice made.
        require(voice_language_path("game:\\sound\\SRN_Eact_SN.csb", "en").empty(), "English stays English");
        require(voice_language_path("game:\\sound\\SRN_BGM.csb", "en").empty(), "music is not a voice file");
        require(voice_language_path("game:\\advJ", "en").empty(), "text files are not voice files");
        require(voice_language_path("game:\\sound\\SRN_stream_voice_jx.cpk", "en").empty(), "only the one letter");
        require(voice_language_path("game:\\sound\\SRN_Jact_SN.csb", "").empty(), "no choice follows the game");
        require(voice_language_path("game:\\sound\\SRN_Jact_SN.csb", "auto").empty(), "auto follows the game");
        std::cout << "voice language checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
