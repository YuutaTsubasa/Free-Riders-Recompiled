#include "voice_commands.h"

namespace sfr {

bool VoiceRecognizer::supported() { return false; }

std::unique_ptr<VoiceRecognizer> VoiceRecognizer::open(const std::vector<std::string>&, std::string* why) {
    if (why) *why = "unsupported-platform";
    return nullptr;
}

}
