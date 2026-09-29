#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {

// The Kinect's voice commands, spoken again. The title listens for words of
// its own vocabulary (nui_speech.h: "start", "ok", "back", "next"...); a
// microphone -- the Kinect's own array is one to Windows -- and the host's
// speech recognizer hear the player say them, in English as on the console,
// or in Traditional Chinese, and the title is given its word.

// Every phrase the recognizer listens for.
const std::vector<std::string>& voice_phrases();
// The title's word for a phrase heard (UTF-8), or empty. "Start" opens the
// pause ring in a race, where the title's word for it is "pauseopen".
std::string_view title_word_for(std::string_view phrase, bool racing);

// The host's speech recognizer, listening for phrases on a thread of its
// own. SFR_VOICE=1 asks for it; SFR_VOICE_LANGUAGE picks the recognizer by
// its language (409 English, 404 Traditional Chinese; the system's own by
// default).
class VoiceRecognizer {
public:
    virtual ~VoiceRecognizer();
    // The oldest phrase heard since the last call, if any.
    virtual bool next(std::string& phrase) = 0;
    // Null, with the reason in why, where there is no recognizer.
    static std::unique_ptr<VoiceRecognizer> open(const std::vector<std::string>& phrases, std::string* why = nullptr);
    static bool supported();
};

}
