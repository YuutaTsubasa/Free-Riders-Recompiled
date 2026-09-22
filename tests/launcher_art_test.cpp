#include "launcher_art.h"
#include "launcher_sound.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

uint32_t le32(const uint8_t* at) { return uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24; }

void icon_is_a_disc() {
    for (const int size : {16, 32, 48, 256}) {
        const auto pixels = sfr::render_icon(size);
        require(pixels.size() == size_t(size) * size_t(size), "the icon has size x size pixels");
        require((pixels[0] >> 24) == 0 && (pixels.back() >> 24) == 0, "the corners are transparent");
        const uint32_t centre = pixels[size_t(size / 2) * size + size / 2];
        require((centre >> 24) == 255, "the centre is opaque");
        int partial = 0;
        for (const uint32_t p : pixels) partial += (p >> 24) > 0 && (p >> 24) < 255;
        require(partial > 0, "the edge is antialiased");
    }
}

void sounds_are_short_valid_waves() {
    for (int i = 0; i < int(sfr::UiSound::count); ++i) {
        const auto wave = sfr::synthesize_ui_sound(sfr::UiSound(i));
        require(wave.size() > 44 && std::memcmp(wave.data(), "RIFF", 4) == 0 && std::memcmp(wave.data() + 8, "WAVEfmt ", 8) == 0,
                "a sound is a RIFF WAVE file");
        require(le32(wave.data() + 4) == wave.size() - 8 && le32(wave.data() + 40) == wave.size() - 44,
                "the RIFF and data sizes match the file");
        require(le32(wave.data() + 24) == 48000 && wave[22] == 1 && wave[34] == 16, "48 kHz 16-bit mono");
        const size_t samples = (wave.size() - 44) / 2;
        require(samples > 48000 / 50 && samples < 48000, "between 20 ms and one second long");
        int peak = 0;
        for (size_t s = 0; s < samples; ++s) {
            const int16_t v = int16_t(wave[44 + s * 2] | wave[45 + s * 2] << 8);
            peak = std::max(peak, std::abs(int(v)));
        }
        require(peak > 2000 && peak < 32000, "audible and not clipped");
        const int16_t last = int16_t(wave[wave.size() - 2] | wave[wave.size() - 1] << 8);
        require(std::abs(int(last)) < 600, "the sound ends near silence (no click)");
    }
}
}

int main() {
    try {
        icon_is_a_disc();
        sounds_are_short_valid_waves();
        std::cout << "Launcher art and sound checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
