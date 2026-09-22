#pragma once
#include <cstdint>
#include <memory>

namespace sfr {
// One frame of the console's audio render driver: 256 samples of six
// channels (front left, front right, centre, LFE, surround left, surround
// right), each channel's 256 big-endian floats after the previous
// channel's (as Xenia reads them).
constexpr uint32_t audio_frame_samples = 256, audio_frame_channels = 6;
constexpr uint32_t audio_frame_bytes = audio_frame_samples * audio_frame_channels * 4;
constexpr uint32_t audio_sample_rate = 48000;

// The frame as interleaved stereo floats (left, right): the centre and the
// surrounds folded in at -3 dB, the LFE left out.
void downmix_audio_frame(const uint8_t* frame, float* stereo);

// Plays the frames on the default output device (XAudio2; SDL elsewhere). A frame is
// dropped rather than queued once about a quarter second is waiting, so
// the sound never falls behind the game.
class NativeAudio {
public:
    // Null when no output device is available.
    static std::unique_ptr<NativeAudio> create();
    virtual ~NativeAudio() = default;
    virtual void submit(const uint8_t* frame) = 0;
};
}
