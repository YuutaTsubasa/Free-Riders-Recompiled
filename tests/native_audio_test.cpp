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
        std::cout << "native audio tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
