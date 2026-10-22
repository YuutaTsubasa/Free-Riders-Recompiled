#include "kinect_depth_compare.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace sfr {

namespace {
constexpr double none = std::numeric_limits<double>::quiet_NaN();
}

double kinect_series_at(const KinectSeries& series, int64_t time, int64_t max_gap_us) {
    const auto& t = series.t;
    if (t.empty() || time < t.front() || time > t.back()) return none;
    const size_t after = size_t(std::lower_bound(t.begin(), t.end(), time) - t.begin());
    if (t[after] == time) return series.v[after];
    const size_t before = after - 1;
    const double a = series.v[before], b = series.v[after];
    if (!std::isfinite(a) || !std::isfinite(b) || t[after] - t[before] > max_gap_us) return none;
    const double f = double(time - t[before]) / double(t[after] - t[before]);
    return a + (b - a) * f;
}

namespace {
// Pearson correlation of the pairs where both are present.
double correlation(const std::vector<double>& a, const std::vector<double>& b, size_t& pairs) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    pairs = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) continue;
        ++pairs;
        sa += a[i];
        sb += b[i];
        saa += a[i] * a[i];
        sbb += b[i] * b[i];
        sab += a[i] * b[i];
    }
    if (pairs < 2) return none;
    const double n = double(pairs);
    const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    // A constant series (to rounding) correlates with nothing.
    if (!(va > 1e-12 * saa) || !(vb > 1e-12 * sbb)) return none;
    return (sab - sa * sb / n) / std::sqrt(va * vb);
}
}

KinectLag kinect_cross_correlate(const KinectSeries& a, const KinectSeries& b, int64_t max_lag_us, int64_t step_us,
                                 size_t min_pairs) {
    KinectLag out;
    if (a.t.empty() || b.t.empty() || step_us <= 0) return out;
    const int64_t start = a.t.front(), end = a.t.back();
    // Samples close enough to stand for the series between them: three
    // steps, or a tenth of a second, whichever is more.
    const int64_t gap = std::max<int64_t>(3 * step_us, 100000);
    std::vector<double> sa;
    for (int64_t t = start; t <= end; t += step_us) sa.push_back(kinect_series_at(a, t, gap));
    const int64_t steps = max_lag_us / step_us;
    std::vector<double> rs(size_t(2 * steps + 1), none);
    std::vector<double> sb(sa.size());
    double best = -2;
    int64_t best_k = 0;
    for (int64_t k = -steps; k <= steps; ++k) {
        for (size_t i = 0; i < sa.size(); ++i) sb[i] = kinect_series_at(b, start + int64_t(i) * step_us + k * step_us, gap);
        size_t pairs = 0;
        const double r = correlation(sa, sb, pairs);
        if (pairs < min_pairs || !std::isfinite(r)) continue;
        rs[size_t(k + steps)] = r;
        if (r > best) {
            best = r;
            best_k = k;
            out.pairs = pairs;
        }
    }
    if (best < -1) return out;
    out.found = true;
    out.r = best;
    double lag = double(best_k);
    const size_t at = size_t(best_k + steps);
    if (at > 0 && at + 1 < rs.size() && std::isfinite(rs[at - 1]) && std::isfinite(rs[at + 1])) {
        const double l = rs[at - 1], c = rs[at], r = rs[at + 1];
        const double denominator = l - 2 * c + r;
        if (denominator < 0) lag += std::clamp(0.5 * (l - r) / denominator, -0.5, 0.5);
    }
    out.lag_us = lag * double(step_us);
    return out;
}

KinectFit kinect_fit(const std::vector<double>& truth, const std::vector<double>& other) {
    KinectFit out;
    double st = 0, so = 0, stt = 0, soo = 0, sto = 0, sd = 0;
    for (size_t i = 0; i < truth.size() && i < other.size(); ++i) {
        const double y = truth[i], x = other[i];
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        ++out.n;
        st += y;
        so += x;
        stt += y * y;
        soo += x * x;
        sto += y * x;
        sd += (y - x) * (y - x);
    }
    if (!out.n) {
        out.r = out.slope = out.intercept = out.rmse = out.residual_rmse = none;
        return out;
    }
    const double n = double(out.n);
    out.mean_truth = st / n;
    out.mean_other = so / n;
    out.rmse = std::sqrt(sd / n);
    const double vo = soo - so * so / n, vt = stt - st * st / n, c = sto - st * so / n;
    // Constant to rounding: no correlation, no slope.
    const bool other_moves = vo > 1e-12 * soo, truth_moves = vt > 1e-12 * stt;
    out.r = other_moves && truth_moves ? c / std::sqrt(vo * vt) : none;
    if (other_moves) {
        out.slope = c / vo;
        out.intercept = out.mean_truth - out.slope * out.mean_other;
    } else {
        out.slope = none;
        out.intercept = out.mean_truth - out.mean_other;
    }
    double residual = 0;
    for (size_t i = 0; i < truth.size() && i < other.size(); ++i) {
        const double y = truth[i], x = other[i];
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        const double fitted = std::isfinite(out.slope) ? out.slope * x + out.intercept : x + out.intercept;
        residual += (y - fitted) * (y - fitted);
    }
    out.residual_rmse = std::sqrt(residual / n);
    return out;
}

KinectDepthSample kinect_depth_sample(const KinectDepthRecordFrame& frame) {
    KinectDepthSample out;
    out.steady_us = frame.steady_us;
    out.number = frame.number;
    const KinectDepthImageStats stats = kinect_depth_image_stats(frame.pixels.data(), frame.pixels.size());
    out.invalid = stats.invalid;
    uint32_t best = 0, slot = 0;
    for (uint32_t index = 1; index <= 6; ++index) {
        out.player_pixels += stats.players[index];
        if (stats.players[index] > best) {
            best = stats.players[index];
            slot = index - 1;
        }
    }
    out.player = best > 0;
    if (out.player) out.features = kinect_depth_view_features(frame.pixels.data(), frame.width, frame.height, slot);
    return out;
}

double kinect_depth_lean(float lower_left, float lower_right) {
    if (lower_left == 0.0f) return 1.0;
    if (lower_left == lower_right) return 0.0;
    if (lower_left > lower_right)
        return lower_right > 0 ? std::min(3.5, double(lower_left) / double(lower_right) - 1.0) : 3.5;
    return -std::min(3.5, double(lower_right) / double(lower_left) - 1.0);
}

const std::vector<std::string> kinect_depth_compare_features = {
    "centroid_col", "centroid_row", "min_col", "min_row", "max_col", "max_row", "player_pixels",
    "count_lower_left", "count_lower_right", "count_upper_left", "count_upper_right", "lean",
    "mean_lower_left", "mean_lower_right", "mean_upper_left", "mean_upper_right", "near_count", "mean_depth",
    "invalid_ratio",
};

double kinect_depth_feature(const KinectDepthSample& s, size_t feature) {
    if (feature == 18) return double(s.invalid);
    if (!s.player) return none;
    const KinectDepthCentroid& c = s.features.centroid;
    const KinectDepthSplit& p = s.features.split;
    switch (feature) {
    case 0: return c.column;
    case 1: return c.row;
    case 2: return c.min_column;
    case 3: return c.min_row;
    case 4: return c.max_column;
    case 5: return c.max_row;
    case 6: return c.pixels;
    case 7: case 8: case 9: case 10: return double(p.counts[feature - 7]);
    case 11: return kinect_depth_lean(p.counts[lower_left], p.counts[lower_right]);
    // An empty part's mean is the title's 65528: not a depth to compare.
    case 12: case 13: case 14: case 15:
        return p.written && p.counts[feature - 12] > 0 ? double(p.means[feature - 12]) : none;
    case 16: return p.written ? double(p.near_count) : none;
    case 17: return p.written ? double(p.average) : none;
    default: return none;
    }
}

std::vector<int64_t> kinect_detector_event_starts(const std::vector<KinectRaceRecord>& rows, size_t detector) {
    std::vector<int64_t> starts;
    if (detector >= kinect_race_detector_count) return starts;
    std::vector<std::pair<uint32_t, int>> previous;  // per body record, its last answer
    for (const KinectRaceRecord& row : rows) {
        const int value = row.original[detector];
        if (value < 0) continue;  // not asked on this update
        auto it = std::find_if(previous.begin(), previous.end(), [&](const auto& p) { return p.first == row.record; });
        if (it == previous.end()) it = previous.insert(previous.end(), {row.record, -1});
        if (value == 1 && it->second != 1) starts.push_back(row.steady_us);
        it->second = value;
    }
    std::sort(starts.begin(), starts.end());
    return starts;
}

KinectEventAmplitudes kinect_event_amplitudes(const std::vector<int64_t>& times, const std::vector<double>& truth,
                                              const std::vector<double>& other, const std::vector<int64_t>& starts,
                                              bool upward, int64_t baseline_from_us, int64_t baseline_to_us,
                                              int64_t response_us) {
    KinectEventAmplitudes out;
    out.events = starts.size();
    double sum_truth = 0, sum_other = 0;
    const auto amplitude = [&](const std::vector<double>& v, int64_t t) {
        double base = 0, peak = upward ? -INFINITY : INFINITY;
        size_t n = 0, m = 0;
        const size_t first = size_t(std::lower_bound(times.begin(), times.end(), t + baseline_from_us) - times.begin());
        for (size_t i = first; i < times.size() && i < v.size() && times[i] <= t + response_us; ++i) {
            if (!std::isfinite(v[i])) continue;
            if (times[i] <= t + baseline_to_us) {
                base += v[i];
                ++n;
            }
            if (times[i] >= t) {
                peak = upward ? std::max(peak, v[i]) : std::min(peak, v[i]);
                ++m;
            }
        }
        return n && m ? peak - base / double(n) : none;
    };
    for (const int64_t t : starts) {
        const double a = amplitude(truth, t), b = amplitude(other, t);
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        ++out.measured;
        sum_truth += a;
        sum_other += b;
        out.same_direction += (a > 0) == (b > 0);
    }
    out.truth = out.measured ? sum_truth / double(out.measured) : none;
    out.other = out.measured ? sum_other / double(out.measured) : none;
    return out;
}

const std::array<const char*, kinect_depth_view_field_count> kinect_depth_view_fields = {
    "centroid_col", "centroid_row", "min_col", "min_row", "max_col", "max_row",
    "count_lower_left", "count_lower_right", "count_upper_left", "count_upper_right",
    "mean_lower_left", "mean_lower_right", "mean_upper_left", "mean_upper_right", "near_count", "mean_depth",
};

bool KinectDepthViewCheck::all_equal() const {
    for (size_t i = 0; i < kinect_depth_view_field_count; ++i)
        if (compared[i] && !equal[i]) return false;
    return true;
}

namespace {
std::string text(uint32_t value) { return std::to_string(value); }
std::string text(float value) {
    char out[32];
    std::snprintf(out, sizeof out, "%.9g", double(value));
    return out;
}
// Bit for bit (a NaN the CSV could not hold is never equal).
bool same(float a, float b) {
    uint32_t x = 0, y = 0;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x == y;
}
}

KinectDepthViewCheck kinect_depth_view_check(const KinectDepthViewRecord& game, const KinectDepthFeatures& tool) {
    KinectDepthViewCheck out;
    const KinectDepthCentroid& g = game.centroid;
    const KinectDepthCentroid& t = tool.centroid;
    const std::array<std::pair<uint32_t, uint32_t>, 6> integers = {{{g.column, t.column}, {g.row, t.row},
        {g.min_column, t.min_column}, {g.min_row, t.min_row}, {g.max_column, t.max_column}, {g.max_row, t.max_row}}};
    for (size_t i = 0; i < 6; ++i) {
        out.compared[i] = true;
        out.equal[i] = integers[i].first == integers[i].second;
        out.game[i] = text(integers[i].first);
        out.tool[i] = text(integers[i].second);
    }
    const KinectDepthSplit& s = tool.split;
    for (size_t i = 0; i < 4; ++i) {
        out.compared[6 + i] = true;
        out.equal[6 + i] = same(game.counts[i], s.counts[i]);
        out.game[6 + i] = text(game.counts[i]);
        out.tool[6 + i] = text(s.counts[i]);
        out.game[10 + i] = text(game.means[i]);
        out.tool[10 + i] = s.written ? text(s.means[i]) : "(not written)";
    }
    out.game[14] = text(game.near_count);
    out.tool[14] = s.written ? text(s.near_count) : "(not written)";
    out.game[15] = text(game.average);
    out.tool[15] = s.written ? text(s.average) : "(not written)";
    if (s.written) {
        for (size_t i = 0; i < 4; ++i) {
            out.compared[10 + i] = true;
            out.equal[10 + i] = same(game.means[i], s.means[i]);
        }
        out.compared[14] = out.compared[15] = true;
        out.equal[14] = game.near_count == s.near_count;
        out.equal[15] = same(game.average, s.average);
    }
    return out;
}

}
