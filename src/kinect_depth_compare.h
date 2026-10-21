#pragma once
#include "kinect_depth_features.h"
#include "kinect_race_record.h"
#include "kinect_record_depth.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sfr {

// Comparing two depth recordings of the same movement at the same time
// (sfr_kinect_depth_compare; docs/kinect-sensor.md): a real Kinect v1's
// images, which the game's depth view reads on a console, against the ones a
// Kinect v2 makes for the game (kinect_v2_depth.h), feature by feature of
// the title's own depth view (kinect_depth_features.h). The two series are
// put on the host's steady clock (both recorders share it), their remaining
// lag found by cross-correlating the vertical centroid, and then each
// feature compared: correlation, the straight line taking the v2's value to
// the v1's (scale and offset), the RMS difference.
//
// None of this is Windows', so it is tested everywhere (kinect_depth_test).

// One time series: times in microseconds (rising), values (NaN: none).
struct KinectSeries {
    std::vector<int64_t> t;
    std::vector<double> v;
    void add(int64_t time, double value) { t.push_back(time); v.push_back(value); }
};

// The value at a time by linear interpolation between the two samples
// around it, both present and at most max_gap_us apart; NaN otherwise. A
// time exactly on a sample takes it.
double kinect_series_at(const KinectSeries& series, int64_t time, int64_t max_gap_us);

// The lag of b behind a, a(t) ~ b(t + lag): both resampled every step_us
// over their common span, then the Pearson correlation of the standardized
// samples for each lag in [-max_lag_us, max_lag_us] (step_us apart), the
// best refined by a parabola through it and its neighbours. found is false
// when too few samples overlap (fewer than min_pairs) or one is constant.
struct KinectLag {
    bool found = false;
    double lag_us = 0;
    double r = 0;      // the correlation at the best whole step
    size_t pairs = 0;  // samples compared at it
};
KinectLag kinect_cross_correlate(const KinectSeries& a, const KinectSeries& b, int64_t max_lag_us, int64_t step_us,
                                 size_t min_pairs = 20);

// truth ~ slope * other + intercept, least squares, over the pairs where both
// are present.
struct KinectFit {
    size_t n = 0;
    double r = 0;              // Pearson correlation (NaN when one is constant)
    double slope = 0, intercept = 0;
    double rmse = 0;           // RMS of truth - other
    double residual_rmse = 0;  // RMS of truth - (slope * other + intercept)
    double mean_truth = 0, mean_other = 0;
};
KinectFit kinect_fit(const std::vector<double>& truth, const std::vector<double>& other);

// A depth file's per-image summary for the comparison: the player is the
// slot with the most pixels in that image (a v1's skeleton slot and a v2's
// body index need not agree, so each file chooses its own).
struct KinectDepthSample {
    int64_t steady_us = 0;
    uint64_t number = 0;
    float invalid = 0;            // pixels without a depth, 0..1
    uint32_t player_pixels = 0;   // pixels of any player
    bool player = false;          // some player in the image
    KinectDepthFeatures features; // of the chosen slot
};
KinectDepthSample kinect_depth_sample(const KinectDepthRecordFrame& frame);

// The race's lean from the counts, as the title reads +640 against +644
// (822C6200, docs/race-controls.md): ll > lr leans right ll/lr - 1, lr > ll
// left (negative) lr/ll - 1, at most 3.5 either way; a zero ll reads as 1.0.
double kinect_depth_lean(float lower_left, float lower_right);

// The compared features, by name, and their value in a sample (NaN when
// the image does not give it: no player, or means of an image too sparse).
extern const std::vector<std::string> kinect_depth_compare_features;
double kinect_depth_feature(const KinectDepthSample& sample, size_t feature);

// The game's own crouches and jumps as events (sfr_kinect_depth_compare
// --events): the start of every run of 1 in a detector's original answer in
// the race CSV (kinect_race_record.h; per body record, rows where it was not
// asked skipped), its steady_us.
std::vector<int64_t> kinect_detector_event_starts(const std::vector<KinectRaceRecord>& rows, size_t detector);

// How far a feature moves at each event, on both sensors' aligned series
// (times rising; truth the v1's, other the v2's, NaN for none): the mean over
// [t + baseline_from, t + baseline_to] is the baseline, the response is the
// largest (upward) or smallest value over [t, t + response] less it. An
// event counts (measured) when both series give both. The means of the
// measured events, and how many moved the same way on both (signs alike).
struct KinectEventAmplitudes {
    size_t events = 0, measured = 0, same_direction = 0;
    double truth = 0, other = 0;  // mean amplitudes (NaN when none measured)
    double ratio() const { return other / truth; }
};
KinectEventAmplitudes kinect_event_amplitudes(const std::vector<int64_t>& times, const std::vector<double>& truth,
                                              const std::vector<double>& other, const std::vector<int64_t>& starts,
                                              bool upward, int64_t baseline_from_us = -2000000,
                                              int64_t baseline_to_us = -500000, int64_t response_us = 1200000);

// Checking the offline features against what the game's depth view wrote
// for the same image (the race CSV's view columns, kinect_race_record.h):
// field by field, exactly (floats bit for bit). The centroid, extents and
// the four counts always; the means, near count and average only when the
// image had enough of the player for the game to write them (otherwise the
// game's memory still holds an older image's).
constexpr size_t kinect_depth_view_field_count = 16;
extern const std::array<const char*, kinect_depth_view_field_count> kinect_depth_view_fields;
struct KinectDepthViewCheck {
    std::array<bool, kinect_depth_view_field_count> compared{}, equal{};
    bool all_equal() const;
    // The two values of a field, as text, for a report.
    std::array<std::string, kinect_depth_view_field_count> game, tool;
};
KinectDepthViewCheck kinect_depth_view_check(const KinectDepthViewRecord& game, const KinectDepthFeatures& tool);

}
