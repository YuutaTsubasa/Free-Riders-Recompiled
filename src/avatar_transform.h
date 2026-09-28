#pragma once
#include <array>
#include <cmath>
#include <mutex>
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
};

// Consume once per presented frame, including frames where the Avatar is
// hidden. No previous camera/scene transform can survive into the next frame.
class AvatarFrameTransforms {
    std::mutex mutex_;
    std::optional<AvatarFrameTransform> frame_;
public:
    void publish(const AvatarFrameTransform& frame) {
        std::lock_guard lock(mutex_);
        for (const auto* matrix : {&frame.world, &frame.view, &frame.projection})
            for (float value : *matrix)
                if (!std::isfinite(value)) { frame_.reset(); return; }
        frame_ = frame;
    }
    std::optional<AvatarFrameTransform> consume() {
        std::lock_guard lock(mutex_);
        auto result = frame_;
        frame_.reset();
        return result;
    }
};
inline AvatarFrameTransforms avatar_frame_transforms;
}
