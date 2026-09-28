#pragma once
#include "avatar_pose.h"
#include <cstdint>
#include <optional>

namespace sfr {
// Guest hooks run under the guest permit. Keep one current clip per local;
// object addresses alone are not a lifetime token across races or presents.
struct AvatarClipPose {
    uint32_t rider = 0, animation = 0, present = 0;
    AvatarPose pose;
    uint32_t manager = 0, renderer = 0, controller = 0;

    std::optional<AvatarPose> current(uint32_t owner, uint32_t buffer, uint32_t frame,
            uint32_t race = 0, uint32_t model = 0, uint32_t evaluator = 0) const {
        if (!pose.valid || !owner || !buffer || owner != rider || buffer != animation || frame != present ||
            race != manager || model != renderer || evaluator != controller)
            return std::nullopt;
        return pose;
    }
};
}
