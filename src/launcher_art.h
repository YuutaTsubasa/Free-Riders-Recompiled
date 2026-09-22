#pragma once
#include <cstdint>
#include <vector>

namespace sfr {
// The launcher's icon, drawn in code (no image is shipped): a blue disc
// with a bright rim and a hover board crossing it, speed lines behind.
// size x size pixels, top row first, each 0xAARRGGBB with straight alpha.
std::vector<uint32_t> render_icon(int size);
}
