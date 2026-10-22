#pragma once

#include <string>

namespace sfr {

// iOS: the shared audio session as the system has it now - category,
// options, whether other audio plays, sample rate, output volume and the
// outputs of the route - for game.log when a player hears nothing (Issue
// #249). SDL sets the category when it opens a device: without
// SDL_HINT_AUDIO_CATEGORY, Playback, mixed with and ducking other audio
// (options 0x3), which silent mode does not mute.
std::string ios_audio_session_summary();

}  // namespace sfr
