#pragma once
#include "avatar_pose.h"
#include <cstdint>
#include <optional>

namespace sfr {
// Guest hooks run under the guest permit. Keep just the current 1P clip;
// object addresses alone are not a lifetime token across races or presents.
struct AvatarClipPose {
    uint32_t rider = 0, animation = 0, present = 0;
    AvatarPose pose;

    std::optional<AvatarPose> current(uint32_t owner, uint32_t buffer, uint32_t frame) const {
        if (!pose.valid || !owner || !buffer || owner != rider || buffer != animation || frame != present)
            return std::nullopt;
        return pose;
    }
};
}
