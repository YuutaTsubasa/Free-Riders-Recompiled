#pragma once
#include <array>

namespace sfr {
struct AvatarBonePose {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> translation{};
};
struct AvatarPose {
    std::array<AvatarBonePose, 72> bones;
    bool valid = false;
    // Native renderer +8 reflects the completed pose across X.
    bool mirrored = false;
};
}
