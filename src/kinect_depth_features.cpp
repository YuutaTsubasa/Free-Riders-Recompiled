#include "kinect_depth_features.h"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <limits>

namespace sfr {

namespace {
constexpr uint32_t lanes = 8;
constexpr uint16_t depth_mask = 0xFFF8;
// An empty part's mean (the vector at 821AC140).
constexpr float empty_mean = 65528.0f;

uint16_t add16(uint16_t a, uint32_t b) { return uint16_t(std::min<uint32_t>(0xFFFF, uint32_t(a) + b)); }
// 32-bit saturating sums of non-negative values come out as the true sum
// clamped, whatever their order (each partial sum is clamped the same way).
uint32_t clamp32(uint64_t value) { return uint32_t(std::min<uint64_t>(value, 0xFFFFFFFFull)); }
}

KinectDepthCentroid kinect_depth_view_centroid(const uint16_t* pixels, uint32_t width, uint32_t height, uint32_t slot) {
    KinectDepthCentroid out;
    const uint16_t player = uint16_t(slot + 1);
    const uint32_t vectors = width >> 3;
    // The splats the code starts from: the smallest column is the width's
    // low halfword, the rows its own counters (82439998..82439B7C).
    std::array<uint16_t, lanes> min_column, max_column{}, count{};
    min_column.fill(uint16_t(width));
    uint64_t column_sum = 0, row_sum = 0;
    uint32_t min_row = height, max_row = 0;
    uint16_t row_lane = 0;  // v3: the row, a saturating 16-bit splat
    const uint16_t* p = pixels;
    for (uint32_t row = 0; row < height; ++row) {
        std::array<uint16_t, lanes> columns{}, rows{}, counts{};
        for (uint32_t v = 0; v < vectors; ++v, p += lanes)
            for (uint32_t lane = 0; lane < lanes; ++lane) {
                const uint16_t column = uint16_t(std::min<uint32_t>(0xFFFF, v * lanes + lane));
                if ((p[lane] & ~depth_mask) != player) continue;
                min_column[lane] = std::min(min_column[lane], column);
                max_column[lane] = std::max(max_column[lane], column);
                columns[lane] = add16(columns[lane], column);
                rows[lane] = add16(rows[lane], row_lane);
                counts[lane] = add16(counts[lane], 1);
            }
        row_lane = add16(row_lane, 1);
        bool any = false;
        for (uint32_t lane = 0; lane < lanes; ++lane) any = any || counts[lane];
        if (!any) continue;
        for (uint32_t lane = 0; lane < lanes; ++lane) {
            count[lane] = add16(count[lane], counts[lane]);
            column_sum += columns[lane];
            row_sum += rows[lane];
        }
        if (row < min_row) min_row = row;
        if (row > max_row) max_row = row;
    }
    uint32_t total = 0;
    for (uint16_t c : count) total += c;
    out.pixels = total;
    if (total) {
        out.column = clamp32(column_sum) / total;
        out.row = clamp32(row_sum) / total;
    } else {
        out.column = width >> 1;
        out.row = height >> 1;
    }
    out.min_column = *std::min_element(min_column.begin(), min_column.end());
    // The largest column as the title takes it: lanes e and e + 4 merged by
    // vminuw (82439D30), not vmaxuw, then the largest of the four. A player
    // whose rightmost columns all fall in lanes whose partner lane saw less
    // gets a smaller "largest column" -- kept, as it is what the game has.
    uint16_t largest = 0;
    for (uint32_t lane = 0; lane < 4; ++lane)
        largest = std::max(largest, std::min(max_column[lane], max_column[lane + 4]));
    out.max_column = largest;
    out.min_row = min_row;
    out.max_row = max_row;
    return out;
}

KinectDepthSplit kinect_depth_view_split(const uint16_t* pixels, uint32_t width, uint32_t height, uint32_t slot,
                                         uint32_t centroid_column, uint32_t centroid_row) {
    KinectDepthSplit out;
    const uint16_t player = uint16_t(slot + 1);
    const uint32_t vectors = width >> 3;
    const uint16_t cc = uint16_t(centroid_column);
    // Per part (KinectDepthPart order) the lanes' counts and the depth sum.
    std::array<std::array<uint16_t, lanes>, 4> counts{};
    std::array<uint64_t, 4> sums{};
    const uint16_t* p = pixels;
    const auto rows = [&](uint32_t first, uint32_t last, uint32_t left, uint32_t right) {
        for (uint32_t row = first; row < last; ++row)
            for (uint32_t v = 0; v < vectors; ++v, p += lanes)
                for (uint32_t lane = 0; lane < lanes; ++lane) {
                    const uint16_t column = uint16_t(std::min<uint32_t>(0xFFFF, v * lanes + lane));
                    if ((p[lane] & ~depth_mask) != player) continue;
                    const uint32_t part = cc > column ? left : right;
                    counts[part][lane] = add16(counts[part][lane], 1);
                    sums[part] += p[lane] & depth_mask;
                }
    };
    // The upper part (rows above the centroid's), then the lower; when the
    // centroid row is 0 or the height, that part's loop does not run.
    rows(0, std::min(centroid_row, height), upper_left, upper_right);
    if (centroid_row != height) rows(std::min(centroid_row, height), height, lower_left, lower_right);
    std::array<uint32_t, 4> count{};
    std::array<float, 4> c{}, s{};
    for (uint32_t part = 0; part < 4; ++part) {
        for (uint16_t n : counts[part]) count[part] += n;
        // fcfid and frsp: the integer through a double into a float.
        c[part] = float(double(count[part]));
        s[part] = float(double(clamp32(sums[part])));
    }
    // ((upper right + upper left) + lower right) + lower left, each a float
    // (fadds 8243A234, 8243A240, 8243A260), against (width * height) >> 9.
    float total = c[upper_right] + c[upper_left];
    total = total + c[lower_right];
    total = total + c[lower_left];
    const uint32_t threshold = (uint32_t(width * height) >> 9) & 0x7FFFFF;
    if (total < float(double(threshold))) return out;  // only the counts, zeroed
    out.written = true;
    out.counts = c;
    for (uint32_t part = 0; part < 4; ++part) {
        // vrefp and two Newton-Raphson steps, as the recompiled code does
        // them: separate multiplies and adds (vnmsubfp is c - a*b).
        const float x = c[part];
        const float r0 = 1.0f / x;
        float t = x * r0;
        float e = 1.0f - t;
        float r1 = r0 * e;
        r1 = r1 + r0;
        const bool refined = r1 == r1;  // not NaN (vcmpeqfp)
        t = x * r1;
        e = 1.0f - t;
        float r2 = r1 * e;
        r2 = r2 + r1;
        const float reciprocal = refined ? r2 : r0;
        const float mean = s[part] * reciprocal;
        out.means[part] = x > 0.0f ? mean : empty_mean;
    }
    // (((UR + UL) + LR) + LL) * 0.25 (the float at 8218FCE4).
    float average = out.means[upper_right] + out.means[upper_left];
    average = average + out.means[lower_right];
    average = average + out.means[lower_left];
    average = average * 0.25f;
    out.average = average;
    // fctidz, the low word, its low halfword & 0xFFF8.
    const double truncated = std::trunc(double(average));
    const int64_t whole = truncated >= 9.2e18 ? std::numeric_limits<int64_t>::max()
                          : truncated <= -9.2e18 ? std::numeric_limits<int64_t>::min()
                                                 : int64_t(truncated);
    const uint16_t reference = uint16_t(uint32_t(uint64_t(whole))) & depth_mask;
    std::array<uint16_t, lanes> close{};
    const size_t vectors_all = (size_t(uint32_t(width * height)) >> 3);
    for (size_t v = 0; v < vectors_all; ++v)
        for (uint32_t lane = 0; lane < lanes; ++lane) {
            const uint16_t value = pixels[v * lanes + lane];
            if ((value & ~depth_mask) != player) continue;
            const uint16_t depth = value & depth_mask;
            const uint16_t difference = reference > depth ? uint16_t(reference - depth) : uint16_t(depth - reference);
            if (int16_t(600) > int16_t(difference)) close[lane] = add16(close[lane], 1);
        }
    for (uint16_t n : close) out.near_count += n;
    return out;
}

KinectDepthFeatures kinect_depth_view_features(const uint16_t* pixels, uint32_t width, uint32_t height, uint32_t slot) {
    KinectDepthFeatures out;
    out.slot = slot;
    out.centroid = kinect_depth_view_centroid(pixels, width, height, slot);
    out.split = kinect_depth_view_split(pixels, width, height, slot, out.centroid.column, out.centroid.row);
    return out;
}

KinectDepthImageStats kinect_depth_image_stats(const uint16_t* pixels, size_t count) {
    KinectDepthImageStats out;
    if (!count) return out;
    size_t invalid = 0;
    for (size_t i = 0; i < count; ++i) {
        invalid += (pixels[i] & depth_mask) == 0;
        ++out.players[pixels[i] & 7];
    }
    out.invalid = float(double(invalid) / double(count));
    return out;
}

}
