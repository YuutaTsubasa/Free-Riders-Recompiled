#include "native_audio.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void put(std::vector<uint8_t>& frame, uint32_t channel, uint32_t sample, float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    uint8_t* at = frame.data() + (size_t(channel) * sfr::audio_frame_samples + sample) * 4;
    at[0] = uint8_t(bits >> 24); at[1] = uint8_t(bits >> 16); at[2] = uint8_t(bits >> 8); at[3] = uint8_t(bits);
}
}

int main() {
    try {
        std::vector<uint8_t> frame(sfr::audio_frame_bytes, 0);
        // Channels are stored one after another, big-endian.
        put(frame, 0, 3, 0.5f);    // front left
        put(frame, 1, 3, -0.25f);  // front right
        put(frame, 2, 7, 1.0f);    // centre
        put(frame, 3, 7, 1.0f);    // LFE, left out
        put(frame, 4, 9, 0.5f);    // surround left
        put(frame, 5, 10, 0.5f);   // surround right
        std::vector<float> stereo(sfr::audio_frame_samples * 2, 99.0f);
        sfr::downmix_audio_frame(frame.data(), stereo.data());
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
        require(near(stereo[6], 0.5f) && near(stereo[7], -0.25f), "front channels go to their sides");
        require(near(stereo[14], 0.70710678f) && near(stereo[15], 0.70710678f), "centre to both at -3 dB, no LFE");
        require(near(stereo[18], 0.35355339f) && near(stereo[19], 0.0f), "surround left to the left");
        require(near(stereo[20], 0.0f) && near(stereo[21], 0.35355339f), "surround right to the right");
        require(near(stereo[0], 0.0f) && near(stereo[511], 0.0f), "silence stays silent");

        // The cushion: play once 3 frames wait, pause to refill when dry.
        sfr::AudioCushion cushion(3);
        auto step = cushion.next(0);
        require(step.submit && !step.start, "the first frame waits for the cushion");
        step = cushion.next(1);
        require(step.submit && !step.start, "so does the second");
        step = cushion.next(2);
        require(step.submit && step.start && !step.pause, "the third starts the device");
        step = cushion.next(2);
        require(step.submit && !step.start && !step.pause && cushion.underruns() == 0, "then each frame queues");
        step = cushion.next(0);
        require(step.submit && step.pause && !step.start && cushion.underruns() == 1, "running dry pauses to refill");
        cushion.next(1);
        require(cushion.next(2).start, "and it plays again with the cushion full");
        step = cushion.next(sfr::AudioCushion::limit);
        require(!step.submit && cushion.dropped() == 1, "a quarter second waiting drops the frame");
        // 0: each frame plays at once and a dry device is only counted.
        sfr::AudioCushion off(0);
        require(off.next(0).start, "without a cushion the first frame plays");
        step = off.next(0);
        require(step.submit && !step.pause && !step.start && off.underruns() == 1, "and a dry device keeps playing");
        std::cout << "native audio tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
