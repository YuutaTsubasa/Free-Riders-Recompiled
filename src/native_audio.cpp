#include "native_audio.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>
#else
#include <SDL.h>
#endif

namespace sfr {
namespace {
float big_endian_float(const uint8_t* at) {
    return std::bit_cast<float>(uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3]);
}
}

void downmix_audio_frame(const uint8_t* frame, float* stereo) {
    constexpr float fold = 0.70710678f;  // -3 dB
    const auto channel = [&](uint32_t index, uint32_t sample) {
        return big_endian_float(frame + (size_t(index) * audio_frame_samples + sample) * 4);
    };
    for (uint32_t sample = 0; sample < audio_frame_samples; ++sample) {
        const float centre = channel(2, sample) * fold;
        stereo[sample * 2] = channel(0, sample) + centre + channel(4, sample) * fold;
        stereo[sample * 2 + 1] = channel(1, sample) + centre + channel(5, sample) * fold;
    }
}

uint32_t AudioCushion::configured() {
    const char* text = std::getenv("SFR_AUDIO_CUSHION");
    if (!text || !*text) return 12;
    return uint32_t(std::clamp(std::atoi(text), 0, int(limit) - 2));
}

AudioCushion::Step AudioCushion::next(uint32_t queued) {
    Step step;
    if (queued >= limit) {
        if (dropped_++ % 100 == 0) std::cerr << "NATIVE_AUDIO_DROPPED count=" << dropped_ << '\n';
        return step;
    }
    if (playing_ && queued == 0) {
        // Ran dry before this frame came: the device was silent meanwhile.
        if (underruns_++ < 20 || underruns_ % 100 == 0)
            std::cerr << "NATIVE_AUDIO_UNDERRUN count=" << underruns_ << " cushion=" << frames_ << '\n';
        if (frames_ > 1) {
            playing_ = false;
            step.pause = true;
        }
    }
    step.submit = true;
    if (!playing_ && queued + 1 >= std::max<uint32_t>(frames_, 1)) {
        playing_ = true;
        step.start = true;
    }
    return step;
}

#ifdef _WIN32
namespace {
class XAudio2Output final : public NativeAudio {
public:
    ~XAudio2Output() override {
        if (voice_) voice_->DestroyVoice();
        if (master_) master_->DestroyVoice();
        if (engine_) engine_->Release();
        if (com_) CoUninitialize();
    }
    bool open() {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (FAILED(XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(engine_->CreateMasteringVoice(&master_))) return false;
        // SFR_VOLUME: percent (the launcher's volume slider).
        if (const char* volume = std::getenv("SFR_VOLUME"))
            master_->SetVolume(float(std::clamp(std::atoi(volume), 0, 100)) / 100.0f);
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = audio_sample_rate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        // Started by the cushion, once it is full.
        return SUCCEEDED(engine_->CreateSourceVoice(&voice_, &format));
    }
    void submit(const uint8_t* frame) override {
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        // The cushion's limit also keeps the buffer reused below out of the queue.
        const auto step = cushion_.next(state.BuffersQueued);
        if (step.pause) voice_->Stop();
        if (!step.submit) return;
        auto& buffer = ring_[next_];
        next_ = (next_ + 1) % ring_.size();
        downmix_audio_frame(frame, buffer.data());
        XAUDIO2_BUFFER submission{};
        submission.AudioBytes = UINT32(buffer.size() * sizeof(float));
        submission.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
        voice_->SubmitSourceBuffer(&submission);
        if (step.start) voice_->Start();
    }
private:
    AudioCushion cushion_{AudioCushion::configured()};
    bool com_ = false;
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    // Buffers must outlive their playback: a ring larger than the queue.
    std::array<std::array<float, audio_frame_samples * 2>, AudioCushion::limit + 2> ring_{};
    size_t next_ = 0;
};
}

std::unique_ptr<NativeAudio> NativeAudio::create() {
    auto output = std::make_unique<XAudio2Output>();
    if (!output->open()) return nullptr;
    return output;
}
#else
namespace {
// SDL's queue: the device pulls the stereo floats on its own thread.
class SdlAudioOutput final : public NativeAudio {
public:
    ~SdlAudioOutput() override {
        if (device_) SDL_CloseAudioDevice(device_);
        if (initialized_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    bool open() {
        initialized_ = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
        if (!initialized_) return false;
        SDL_AudioSpec wanted{};
        wanted.freq = int(audio_sample_rate);
        wanted.format = AUDIO_F32SYS;
        wanted.channels = 2;
        wanted.samples = audio_frame_samples * 2;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);  // SDL converts if it must
        if (!device_) return false;
        // SFR_VOLUME: percent (the launcher's volume slider).
        if (const char* volume = std::getenv("SFR_VOLUME")) volume_ = float(std::clamp(std::atoi(volume), 0, 100)) / 100.0f;
        return true;  // unpaused by the cushion, once it is full
    }
    void submit(const uint8_t* frame) override {
        // Whole frames: a partly played one counts as still queued.
        const uint32_t queued = uint32_t((SDL_GetQueuedAudioSize(device_) + sizeof(buffer_) - 1) / sizeof(buffer_));
        const auto step = cushion_.next(queued);
        if (step.pause) SDL_PauseAudioDevice(device_, 1);
        if (!step.submit) return;
        downmix_audio_frame(frame, buffer_.data());
        if (volume_ != 1.0f) for (float& sample : buffer_) sample *= volume_;
        SDL_QueueAudio(device_, buffer_.data(), Uint32(sizeof(buffer_)));
        if (step.start) SDL_PauseAudioDevice(device_, 0);
    }
private:
    AudioCushion cushion_{AudioCushion::configured()};
    bool initialized_ = false;
    SDL_AudioDeviceID device_ = 0;
    float volume_ = 1.0f;
    std::array<float, audio_frame_samples * 2> buffer_{};
};
}

std::unique_ptr<NativeAudio> NativeAudio::create() {
    auto output = std::make_unique<SdlAudioOutput>();
    if (!output->open()) return nullptr;
    return output;
}
#endif
}
