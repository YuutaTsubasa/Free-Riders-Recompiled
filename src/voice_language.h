#pragma once
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace sfr {
// The recorded voices apart from the text (SFR_VOICE_LANGUAGE: "en" or "ja";
// anything else follows the game's language). The disc has English and
// Japanese voices: the characters' lines in sound/SRN_Eact_*.csb and
// SRN_Jact_*.csb, the story, announcer and Omochao in
// sound/SRN_stream_voice_e and _j (.csb and .cpk). The game picks the set by
// its language and plays cues by number; the act sheets number their cues
// alike, and the stream sheets share all but a few (67 English-only avatar
// lines, 10 Japanese-only announcements), which go silent in the other set.
// For a path the game opens, the path of the same file in the wanted set, or
// empty when it is not a voice file of the other set.
inline std::string voice_language_path(std::string_view path, std::string_view voice) {
    if (voice != "en" && voice != "ja") return {};
    const bool to_english = voice == "en";
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; };
    const size_t slash = path.find_last_of("/\\");
    const size_t name_at = slash == std::string_view::npos ? 0 : slash + 1;
    std::string out(path);
    auto starts = [&](std::string_view prefix) {
        if (path.size() - name_at < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (lower(path[name_at + i]) != prefix[i]) return false;
        return true;
    };
    // SRN_Jact_SN.csb <-> SRN_Eact_SN.csb: the letter after "SRN_".
    if (starts(to_english ? "srn_jact_" : "srn_eact_")) {
        const char from = path[name_at + 4];
        out[name_at + 4] = to_english ? (from == 'j' ? 'e' : 'E') : (from == 'e' ? 'j' : 'J');
        return out;
    }
    // SRN_stream_voice_j.cpk <-> SRN_stream_voice_e.cpk: the letter before the extension.
    if (starts("srn_stream_voice_")) {
        const size_t letter = name_at + 17;
        if (letter < path.size() && lower(path[letter]) == (to_english ? 'j' : 'e') &&
            (letter + 1 == path.size() || path[letter + 1] == '.')) {
            const char from = path[letter];
            out[letter] = to_english ? (from == 'j' ? 'e' : 'E') : (from == 'e' ? 'j' : 'J');
            return out;
        }
    }
    return {};
}

// The guest path to open for path: the other voice set's file when
// SFR_VOICE_LANGUAGE asks for it (the first redirects are logged), else path.
inline std::string voice_redirected(std::string_view path) {
    static const std::string voice = [] {
        const char* text = std::getenv("SFR_VOICE_LANGUAGE");
        return std::string(text ? text : "");
    }();
    std::string other = voice_language_path(path, voice);
    if (other.empty()) return std::string(path);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        std::cerr << "VOICE_LANGUAGE voice=" << voice << " game=" << path << " opened=" << other << '\n';
    return other;
}
}
