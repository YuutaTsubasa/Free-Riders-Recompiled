#pragma once

#include <string>

namespace sfr {

// iOS: the shared audio session as the system has it now - category,
// options, whether other audio plays, sample rate, output volume and the
// outputs of the route - for game.log when a player hears nothing (Issue
// #249). SDL sets the category when it opens a device (src/native_audio.cpp).
std::string ios_audio_session_summary();

}  // namespace sfr
