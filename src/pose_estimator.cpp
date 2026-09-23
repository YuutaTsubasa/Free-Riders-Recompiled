// The parts of the pose estimate that do not need the runtime: where the
// picture is cropped to the model's input, and how the model's two score
// rows per point become a place in the picture.
#include "pose_estimator.h"

#include <algorithm>
#include <cmath>

namespace sfr {

PoseEstimator::~PoseEstimator() = default;

PoseCrop pose_crop(uint32_t picture_width, uint32_t picture_height, uint32_t model_width, uint32_t model_height) {
    PoseCrop crop;
    if (!picture_width || !picture_height || !model_width || !model_height) return crop;
    // The whole picture, letterboxed: the model sees it all rather than a
    // detector's box, which is what a player standing in front of the camera
    // wants. The larger of the two ratios decides, so nothing is cut off.
    const float scale = (std::max)(float(picture_width) / float(model_width),
                                   float(picture_height) / float(model_height));
    crop.scale_x = crop.scale_y = scale;
    crop.origin_x = (float(picture_width) - float(model_width) * scale) * 0.5f;
    crop.origin_y = (float(picture_height) - float(model_height) * scale) * 0.5f;
    return crop;
}

void decode_simcc(const float* simcc_x, const float* simcc_y, uint32_t points, uint32_t width_bins,
                  uint32_t height_bins, float split_ratio, const PoseCrop& crop, PoseLandmarks& landmarks) {
    if (!simcc_x || !simcc_y || split_ratio <= 0) return;
    const uint32_t limit = (std::min)(points, uint32_t(landmarks.size()));
    for (uint32_t point = 0; point < limit; ++point) {
        const float* row_x = simcc_x + size_t(point) * width_bins;
        const float* row_y = simcc_y + size_t(point) * height_bins;
        const auto best = [](const float* row, uint32_t length) {
            uint32_t at = 0;
            float value = row ? row[0] : 0;
            for (uint32_t i = 1; i < length; ++i)
                if (row[i] > value) { value = row[i]; at = i; }
            return std::pair{at, value};
        };
        const auto [x_bin, x_score] = best(row_x, width_bins);
        const auto [y_bin, y_score] = best(row_y, height_bins);
        PoseLandmark& landmark = landmarks[point];
        landmark.score = (std::min)(x_score, y_score);
        landmark.x = crop.origin_x + float(x_bin) / split_ratio * crop.scale_x;
        landmark.y = crop.origin_y + float(y_bin) / split_ratio * crop.scale_y;
    }
}

}
