#include "launcher_sound.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(SFR_UI_SOUND_SDL)
#include <SDL.h>
#endif

namespace sfr {
namespace {
constexpr uint32_t rate = 48000;
constexpr double pi = 3.14159265358979323846;

// One tone: frequency glides from start to end, a 3 ms attack, then an
// exponential decay; triangle mixes in a softened edge.
struct Tone {
    double at, length, start_hz, end_hz, gain, decay, triangle = 0.0;
};

void mix(std::vector<float>& samples, const Tone& tone) {
    const size_t first = size_t(tone.at * rate), count = size_t(tone.length * rate);
    if (samples.size() < first + count) samples.resize(first + count);
    double phase = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double t = double(i) / rate, progress = t / tone.length;
        const double hz = tone.start_hz + (tone.end_hz - tone.start_hz) * progress;
        phase += 2.0 * pi * hz / rate;
        const double sine = std::sin(phase);
        const double tri = 2.0 / pi * std::asin(sine);
        const double attack = std::min(1.0, t / 0.003);
        const double release = std::min(1.0, (tone.length - t) / 0.004);  // no click at the end
        const double envelope = attack * release * std::exp(-t * tone.decay);
        samples[first + i] += float(tone.gain * envelope * (sine * (1.0 - tone.triangle) + tri * tone.triangle));
    }
}

std::vector<float> render(UiSound sound) {
    std::vector<float> samples;
    switch (sound) {
    case UiSound::move:
        mix(samples, {0.0, 0.045, 1700, 2100, 0.20, 60});
        break;
    case UiSound::confirm:
        mix(samples, {0.0, 0.07, 880, 900, 0.22, 30, 0.4});
        mix(samples, {0.055, 0.14, 1320, 1330, 0.22, 22, 0.4});
        break;
    case UiSound::back:
        mix(samples, {0.0, 0.12, 1180, 640, 0.22, 25, 0.3});
        break;
    case UiSound::toggle_on:
        mix(samples, {0.0, 0.05, 1320, 1560, 0.20, 45, 0.5});
        break;
    case UiSound::toggle_off:
        mix(samples, {0.0, 0.05, 1100, 900, 0.20, 45, 0.5});
        break;
    case UiSound::complete: {
        const double notes[] = {1046.5, 1318.5, 1568.0, 2093.0};
        for (int i = 0; i < 4; ++i) mix(samples, {0.085 * i, 0.7 - 0.085 * i, notes[i], notes[i], 0.15, 5.5, 0.25});
        break;
    }
    case UiSound::error:
        mix(samples, {0.0, 0.11, 330, 320, 0.25, 18, 0.7});
        mix(samples, {0.12, 0.16, 262, 255, 0.25, 14, 0.7});
        break;
    case UiSound::count:
        break;
    }
    return samples;
}

void put16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& out, uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); }
void tag(std::vector<uint8_t>& out, const char* four) { out.insert(out.end(), four, four + 4); }
}

std::vector<uint8_t> synthesize_ui_sound(UiSound sound) {
    const auto samples = render(sound);
    const uint32_t data_bytes = uint32_t(samples.size() * 2);
    std::vector<uint8_t> wave;
    wave.reserve(44 + data_bytes);
    tag(wave, "RIFF");
    put32(wave, 36 + data_bytes);
    tag(wave, "WAVE");
    tag(wave, "fmt ");
    put32(wave, 16);
    put16(wave, 1);  // PCM
    put16(wave, 1);  // mono
    put32(wave, rate);
    put32(wave, rate * 2);
    put16(wave, 2);
    put16(wave, 16);
    tag(wave, "data");
    put32(wave, data_bytes);
    for (const float sample : samples)
        put16(wave, uint16_t(int16_t(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f))));
    return wave;
}

UiSounds::UiSounds() {
    for (size_t i = 0; i < waves_.size(); ++i) waves_[i] = synthesize_ui_sound(UiSound(i));
}

void UiSounds::play(UiSound sound) const {
#ifdef _WIN32
    if (!enabled) return;
    PlaySoundW(reinterpret_cast<LPCWSTR>(waves_[size_t(sound)].data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#elif defined(SFR_UI_SOUND_SDL)
    // The SDL launcher (Linux, Android): the PCM after the wave's data chunk
    // header, queued on one device opened at the first sound.
    static const SDL_AudioDeviceID device = [] {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return SDL_AudioDeviceID(0);
        SDL_AudioSpec wanted{};
        wanted.freq = int(rate);
        wanted.format = AUDIO_S16LSB;
        wanted.channels = 1;
        wanted.samples = 1024;
        const SDL_AudioDeviceID opened = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
        if (opened) SDL_PauseAudioDevice(opened, 0);
        return opened;
    }();
    if (!enabled || !device) return;
    const auto& wave = waves_[size_t(sound)];
    for (size_t at = 12; at + 8 <= wave.size();) {
        const uint32_t size = uint32_t(wave[at + 4]) | uint32_t(wave[at + 5]) << 8 | uint32_t(wave[at + 6]) << 16 |
                              uint32_t(wave[at + 7]) << 24;
        if (std::memcmp(wave.data() + at, "data", 4) == 0) {
            SDL_ClearQueuedAudio(device);
            SDL_QueueAudio(device, wave.data() + at + 8, uint32_t(std::min<size_t>(size, wave.size() - at - 8)));
            return;
        }
        at += 8 + size;
    }
#else
    (void)sound;
#endif
}
}
