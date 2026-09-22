#include "video_mode.h"
#include <bit>
#include <cmath>

namespace sfr {
namespace {
void validate(DisplayMode mode) {
    if (!mode.width || mode.width > 16384 || !mode.height || mode.height > 16384 ||
        !std::isfinite(mode.refresh_hz) || mode.refresh_hz <= 1.0f || mode.refresh_hz > 1000.0f ||
        mode.interlaced)
        throw RuntimeStop("video-mode", 0, "invalid or unsupported native display mode");
}
}

VideoMode::VideoMode(GuestMemory& memory, DisplayMode mode) : memory_(memory), mode_(mode) {
    validate(mode_);
}

std::array<uint32_t, 12> VideoMode::snapshot() const {
    const bool widescreen = uint64_t(mode_.width) * 3 > uint64_t(mode_.height) * 4;
    return {mode_.width, mode_.height, 0, widescreen ? 1u : 0u, mode_.height >= 720 ? 1u : 0u,
            std::bit_cast<uint32_t>(mode_.refresh_hz), 1, 0x4a, 1, 0, 0, 0};
}

void VideoMode::query(uint32_t output) const {
    if (!output) throw RuntimeStop("video-mode", 0, "video mode output must be non-null");
    memory_.check_write(uint64_t(output), structure_size);
    const auto words = snapshot();
    for (size_t i = 0; i < words.size(); ++i)
        memory_.store<uint32_t>(uint64_t(output) + uint64_t(i) * 4, words[i]);
}
}
