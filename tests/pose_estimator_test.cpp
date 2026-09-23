#include "pose_estimator.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float slack = 0.01f) { return std::fabs(a - b) <= slack; }

void the_crop_keeps_the_picture_whole() {
    // A 16:9 picture into the model's 3:4 input: the crop is as wide as the
    // picture and taller than it, so nothing is cut off sideways.
    const sfr::PoseCrop wide = sfr::pose_crop(1280, 720, 192, 256);
    require(near(wide.scale_x, 1280.0f / 192.0f) && near(wide.scale_y, wide.scale_x), "the larger ratio decides");
    require(near(wide.origin_x, 0.0f), "a wide picture is not cropped sideways");
    require(wide.origin_y < 0, "it reaches above and below the picture instead");
    require(near(wide.origin_y + 256 * wide.scale_y, 720 - wide.origin_y, 0.1f), "and by the same amount each way");

    const sfr::PoseCrop tall = sfr::pose_crop(480, 800, 192, 256);
    require(near(tall.scale_y, 800.0f / 256.0f) && near(tall.origin_y, 0.0f), "a tall picture is not cropped up or down");
    require(tall.origin_x < 0, "it reaches past the sides instead");

    const sfr::PoseCrop none = sfr::pose_crop(0, 720, 192, 256);
    require(near(none.scale_x, 1.0f) && near(none.origin_x, 0.0f), "an empty picture gives the identity");
}

void the_decode_takes_the_best_column_of_each_row() {
    constexpr uint32_t points = 3, width_bins = 8, height_bins = 6;
    std::vector<float> x(points * width_bins, 0.0f), y(points * height_bins, 0.0f);
    // Point 0 at column 4 and row 2, point 1 at column 1 and row 5, point 2
    // nowhere in particular (a flat row, so the first column wins).
    x[0 * width_bins + 4] = 0.9f;  y[0 * height_bins + 2] = 0.8f;
    x[1 * width_bins + 1] = 0.4f;  y[1 * height_bins + 5] = 0.7f;
    sfr::PoseCrop crop;
    crop.origin_x = 10; crop.origin_y = 20; crop.scale_x = 2; crop.scale_y = 3;
    sfr::PoseLandmarks landmarks{};
    sfr::decode_simcc(x.data(), y.data(), points, width_bins, height_bins, 2.0f, crop, landmarks);

    require(near(landmarks[0].x, 10 + 4 / 2.0f * 2) && near(landmarks[0].y, 20 + 2 / 2.0f * 3),
            "a point maps back through the crop");
    require(near(landmarks[0].score, 0.8f), "its score is the lesser of the two rows");
    require(near(landmarks[1].x, 10 + 1 / 2.0f * 2) && near(landmarks[1].y, 20 + 5 / 2.0f * 3),
            "the second point too");
    require(near(landmarks[1].score, 0.4f), "and its score");
    require(near(landmarks[2].score, 0.0f), "a flat row is no confidence at all");

    sfr::PoseLandmarks untouched{};
    sfr::decode_simcc(nullptr, y.data(), points, width_bins, height_bins, 2.0f, crop, untouched);
    require(near(untouched[0].score, 0.0f), "no scores, no landmarks");
    sfr::decode_simcc(x.data(), y.data(), points, width_bins, height_bins, 0.0f, crop, untouched);
    require(near(untouched[0].score, 0.0f), "a split ratio of zero is refused");
}
}

int main() {
    try {
        the_crop_keeps_the_picture_whole();
        the_decode_takes_the_best_column_of_each_row();
        std::cout << "Pose estimator checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
