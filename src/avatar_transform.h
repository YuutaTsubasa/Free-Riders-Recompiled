#pragma once
#include "avatar_pose.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <optional>

namespace sfr {
using AvatarMatrix = std::array<float, 16>;
inline AvatarMatrix multiply_avatar_matrices(const AvatarMatrix& a, const AvatarMatrix& b) {
    AvatarMatrix result{};
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            for (size_t inner = 0; inner < 4; ++inner)
                result[row * 4 + column] += a[row * 4 + inner] * b[inner * 4 + column];
    return result;
}

// The guest uses row vectors. These bytes are also the column-major encoding
// of the transposed matrix consumed by model.hlsl's matrix * column vector.
inline AvatarMatrix avatar_model_to_clip(const AvatarMatrix& world, const AvatarMatrix& view,
                                         const AvatarMatrix& projection, float scale, float lowest_y) {
    const AvatarMatrix local{scale,0,0,0, 0,scale,0,0, 0,0,scale,0, 0,-lowest_y*scale,0,1};
    return multiply_avatar_matrices(multiply_avatar_matrices(multiply_avatar_matrices(local, world), view), projection);
}

struct AvatarFrameTransform {
    AvatarMatrix world, view, projection;
    AvatarPose pose;
    uint64_t present = 0;
};

// Called from the native character draw, while its viewport is active.
void draw_avatar_model(const AvatarFrameTransform& frame);
std::optional<AvatarMatrix> avatar_hand_transform(const AvatarPose& pose, uint32_t bone);
inline float avatar_model_scale() {
    static const float scale = [] {
        const char* text = std::getenv("SFR_AVATAR_MODEL_SCALE");
        const double value = text ? std::strtod(text, nullptr) : 1.0;
        return float(std::isfinite(value) && value > 0 && value <= 100 ? value : 1.0);
    }();
    return scale;
}

}
