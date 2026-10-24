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

// When each frame is played, from the frames the device has queued. The
// game makes a frame every 5.33 ms on a host timer; with nothing queued
// ahead, any later frame left the device without sound for a moment, a
// crackle. So the output waits for a cushion (SFR_AUDIO_CUSHION frames,
// default 12, about 64 ms) before it plays, and pauses to build it again
// when it runs dry. A frame is dropped rather than queued once about a
// quarter second is waiting, so the sound never falls behind the game.
// SFR_AUDIO_CUSHION=0 plays each frame at once, as before.
class AudioCushion {
public:
    static constexpr uint32_t limit = 46;
    explicit AudioCushion(uint32_t frames) : frames_(frames) {}
    static uint32_t configured();  // SFR_AUDIO_CUSHION, or 12
    struct Step {
        bool submit = false, start = false, pause = false;
    };
    Step next(uint32_t queued);
    uint64_t underruns() const { return underruns_; }
    uint64_t dropped() const { return dropped_; }
    // The fewest and most frames queued ahead of a frame since the last
    // call (game.log's NATIVE_AUDIO_FRAMES): a minimum near 0 is a device
    // about to run dry, a growing maximum a device playing slower than
    // the frames come.
    struct Window { uint32_t queued_min = 0, queued_max = 0; };
    Window take_window();
private:
    uint32_t frames_;
    bool playing_ = false;
    uint64_t underruns_ = 0, dropped_ = 0;
    uint32_t queued_min_ = UINT32_MAX, queued_max_ = 0;
};

// What the frames themselves held, for game.log: silent frames (every
// sample 0) and jumps (from one stereo sample to the next by more than
// half of full scale, the shape of a click; music and effects rarely do).
struct AudioContent {
    uint64_t silent = 0, jumps = 0;
    float last_left = 0.0f, last_right = 0.0f;
    void count(const float* stereo);  // audio_frame_samples stereo pairs
};

// Plays the frames on the default output device (XAudio2; SDL elsewhere),
// as AudioCushion says.
class NativeAudio {
public:
    // Null when no output device is available.
    static std::unique_ptr<NativeAudio> create();
    virtual ~NativeAudio() = default;
    virtual void submit(const uint8_t* frame) = 0;
    virtual AudioCushion& cushion() = 0;
};
}
