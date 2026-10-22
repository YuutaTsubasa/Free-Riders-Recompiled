#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfr {
// The launcher's interface sounds, synthesized (no recorded sound is
// shipped): short sine and triangle tones with fast envelopes.
enum class UiSound { move, confirm, back, toggle_on, toggle_off, complete, error, count };

// A complete 48 kHz 16-bit mono PCM .wav file for the sound.
std::vector<uint8_t> synthesize_ui_sound(UiSound sound);

// Plays them asynchronously through the default output device; a new sound
// replaces the one playing.
class UiSounds {
public:
    UiSounds();
    void play(UiSound sound) const;
    bool enabled = true;
private:
    std::array<std::vector<uint8_t>, size_t(UiSound::count)> waves_;
};

// Closes the SDL launcher's sound device for good. On iOS the game runs in
// the launcher's process, where SDL opens only one output device: while the
// launcher's stayed open, the game's failed to open and the game played
// without sound (Issue #249).
void release_ui_sound_device();
}
