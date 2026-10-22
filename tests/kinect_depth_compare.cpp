// How well a Kinect v2's depth for the game stands in for a real Kinect v1's:
// two depth recordings of the same movement at the same time (the game on
// the v1, sfr_kinect_probe on the v2; kinect_record_depth.h), compared
// feature by feature of the title's own depth view (kinect_depth_compare.h).
//
//   sfr_kinect_depth_compare <v1 depth.bin> <v2 depth.bin> [--skeleton <v1 skeleton.csv> <v2 skeleton.csv>]
//                            [--csv aligned.csv] [--report report.txt] [--max-lag-ms 1000] [--gap-ms 100]
//                            [--events <v1game-race.csv>]
//
// Both files' images are stamped with the host's steady clock (steady_us),
// which every process of the machine shares, so they are first put on it as
// they are. What is left (each sensor's own latency, the v1's images reaching
// the game every fourth game frame) is found by cross-correlating the
// vertical centroid of the player (pass one's row), resampled every 10 ms,
// over lags of up to --max-lag-ms: the lag is how much later the v2's
// movement shows than the v1's. --skeleton does the same with the two
// skeleton recordings' SpineBase height, as a check of that lag.
//
// Then, at each v1 image, the v2's value at the same moment (plus the lag;
// linear between its two images around it, when they are at most --gap-ms
// apart and both have a player) and per feature: n, Pearson r, the least
// squares line v1 = slope * v2 + intercept, the RMS difference, the RMS left
// after the line, and the feature's own best lag. Also each sensor's share
// of pixels with a depth, its player pixels, and the share of images where
// either lost the player. The report (Traditional Chinese labels, English
// names) goes to the console and --report; --csv gets the aligned series.
//
// --events takes the game's race CSV of the v1 session: every crouch and
// jump the game's own detectors found (runs of 1 in crouch_original and
// jump_original, at the run's first row) is an event, and for both sensors
// the vertical centroid's and the top row's (min_row) amplitude around it:
// the mean over 2.0 to 0.5 s before as the baseline, the largest (crouch) or
// smallest (jump) value over the 1.2 s after less it, on the aligned series
// (kinect_event_amplitudes). Printed: the mean amplitudes, their ratio v2/v1
// and how many events moved the same way on both.
// The last line, "SUMMARY key=value ...", gathers the headline numbers for
// scripts (rec_session.ps1 -Analyse).
#include "kinect_depth_compare.h"
#include "kinect_record.h"
#include "kinect_record_depth.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
struct Recording {
    std::string path, source;
    std::vector<sfr::KinectDepthSample> samples;
    uint32_t width = 0, height = 0;
    bool truncated = false;
};

bool load(const std::string& path, Recording& out) {
    sfr::KinectDepthReader reader;
    if (!reader.open(path)) {
        std::cerr << path << ": " << reader.error() << '\n';
        return false;
    }
    out.path = path;
    out.width = reader.width();
    out.height = reader.height();
    sfr::KinectDepthRecordFrame frame;
    while (reader.next(frame)) {
        out.source = sfr::kinect_depth_source_name(frame.source);
        out.samples.push_back(sfr::kinect_depth_sample(frame));
    }
    out.truncated = reader.truncated();
    if (!reader.error().empty()) std::cerr << path << ": stopped at " << reader.error() << '\n';
    // Rising times, as the series need (a recorder writes them so).
    for (size_t i = 1; i < out.samples.size(); ++i)
        if (out.samples[i].steady_us < out.samples[i - 1].steady_us) {
            std::cerr << path << ": images out of time order at " << i << '\n';
            return false;
        }
    return !out.samples.empty();
}

sfr::KinectSeries series(const Recording& r, size_t feature) {
    sfr::KinectSeries s;
    for (const auto& sample : r.samples) s.add(sample.steady_us, sfr::kinect_depth_feature(sample, feature));
    return s;
}

// A skeleton recording's SpineBase height for one sensor, the nearest body
// of each frame (NaN without one).
sfr::KinectSeries skeleton_series(const std::string& path, const std::string& sensor, bool& ok) {
    sfr::KinectSeries s;
    std::ifstream in(path);
    ok = bool(in);
    std::string line;
    int64_t time = INT64_MIN;
    double y = NAN, z = INFINITY;
    const auto flush = [&] {
        if (time != INT64_MIN) s.add(time, y);
    };
    while (std::getline(in, line)) {
        sfr::KinectRecordRow row;
        if (!sfr::kinect_parse_skeleton_row(line, row) || row.frame.sensor != sensor) continue;
        if (row.frame.steady_us != time) {
            flush();
            time = row.frame.steady_us;
            y = NAN;
            z = INFINITY;
        }
        const auto& joint = row.body.joints[0];
        if (row.has_body && joint.state == 2 && joint.position[2] < z) {
            z = joint.position[2];
            y = joint.position[1];
        }
    }
    flush();
    return s;
}

std::string format(const char* pattern, double value) {
    if (!std::isfinite(value)) return "-";
    char text[64];
    std::snprintf(text, sizeof text, pattern, value);
    return text;
}
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> paths, skeletons;
    std::string csv, report_path, events_path;
    double max_lag_ms = 1000, gap_ms = 100;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--skeleton" && i + 2 < argc) {
            skeletons = {argv[i + 1], argv[i + 2]};
            i += 2;
        } else if (arg == "--csv" && i + 1 < argc) {
            csv = argv[++i];
        } else if (arg == "--report" && i + 1 < argc) {
            report_path = argv[++i];
        } else if (arg == "--max-lag-ms" && i + 1 < argc) {
            max_lag_ms = std::atof(argv[++i]);
        } else if (arg == "--gap-ms" && i + 1 < argc) {
            gap_ms = std::atof(argv[++i]);
        } else if (arg == "--events" && i + 1 < argc) {
            events_path = argv[++i];
        } else {
            paths.push_back(arg);
        }
    }
    if (paths.size() != 2 || !(max_lag_ms > 0) || !(gap_ms > 0)) {
        std::cerr << "usage: sfr_kinect_depth_compare <v1 depth.bin> <v2 depth.bin> [--skeleton <v1.csv> <v2.csv>]\n"
                     "                                [--csv aligned.csv] [--report report.txt] [--max-lag-ms 1000] [--gap-ms 100]\n"
                     "                                [--events <v1game-race.csv>]\n";
        return 2;
    }
    Recording a, b;
    if (!load(paths[0], a) || !load(paths[1], b)) {
        std::cerr << "cannot read both depth recordings\n";
        return 1;
    }
    std::string text;
    const auto line = [&](const std::string& s) { text += s + "\n"; };
    const auto describe = [&](const char* label, const Recording& r) {
        const double seconds = double(r.samples.back().steady_us - r.samples.front().steady_us) / 1e6;
        char out[512];
        std::snprintf(out, sizeof out, "%s %s: source=%s %ux%u images=%zu 長度 %.1f s (%.1f/s)%s", label, r.path.c_str(),
                      r.source.c_str(), r.width, r.height, r.samples.size(), seconds,
                      seconds > 0 ? double(r.samples.size() - 1) / seconds : 0.0, r.truncated ? " 最後一張不完整" : "");
        line(out);
    };
    line("== Kinect 深度比較 (v1 = 標準答案, v2 = 合成深度) ==");
    describe("v1", a);
    describe("v2", b);
    // The game falls back to a Kinect v2 when no v1 opens: then its file is
    // no reference at all.
    if (a.source != "v1") line("警告：第一個檔的來源是 " + a.source + "，不是真正的 Kinect v1（遊戲沒有開到 v1？），它不能當標準答案。");
    if (b.source != "v2") line("警告：第二個檔的來源是 " + b.source + "，不是 Kinect v2 合成的深度。");
    const int64_t overlap_start = std::max(a.samples.front().steady_us, b.samples.front().steady_us);
    const int64_t overlap_end = std::min(a.samples.back().steady_us, b.samples.back().steady_us);
    if (overlap_end <= overlap_start) {
        line("兩個錄製在主機時鐘 (steady_us) 上沒有重疊：不是同一時間錄的，無法比較。");
        std::cout << text;
        return 1;
    }
    line("主機時鐘重疊 overlap: " + format("%.1f", double(overlap_end - overlap_start) / 1e6) + " s");

    // The lag from the vertical centroid.
    const int64_t max_lag = int64_t(max_lag_ms * 1000), step = 10000, gap = int64_t(gap_ms * 1000);
    const sfr::KinectLag lag = sfr::kinect_cross_correlate(series(a, 1), series(b, 1), max_lag, step);
    const double lag_us = lag.found ? lag.lag_us : 0.0;
    line("時間差 lag (centroid_row 互相關): " +
         (lag.found ? format("%+.1f", lag_us / 1000.0) + " ms (v2 比 v1 晚為正), r=" + format("%.3f", lag.r) +
                          ", pairs=" + std::to_string(lag.pairs)
                    : std::string("找不到 (重疊的有玩家影像太少或重心沒有移動)，以 0 ms 對齊")));
    if (lag.found && std::fabs(lag_us) >= double(max_lag) - double(step))
        line("  注意：時間差落在搜尋範圍邊緣，請加大 --max-lag-ms 再跑一次。");
    if (skeletons.size() == 2) {
        bool ok_a = false, ok_b = false;
        const sfr::KinectSeries sa = skeleton_series(skeletons[0], "v1", ok_a);
        const sfr::KinectSeries sb = skeleton_series(skeletons[1], "v2", ok_b);
        if (!ok_a || !ok_b) {
            line("骨架檔讀不到：" + (!ok_a ? skeletons[0] : skeletons[1]));
        } else {
            const sfr::KinectLag skeleton = sfr::kinect_cross_correlate(sa, sb, max_lag, step);
            line("骨架時間差 skeleton lag (spine_base_y): " +
                 (skeleton.found ? format("%+.1f", skeleton.lag_us / 1000.0) + " ms, r=" + format("%.3f", skeleton.r) +
                                       ", pairs=" + std::to_string(skeleton.pairs)
                                 : std::string("找不到")) +
                 "  (v1 frames " + std::to_string(sa.t.size()) + ", v2 frames " + std::to_string(sb.t.size()) + ")");
        }
    }

    // Each v1 image against the v2 at the same moment.
    const size_t features = sfr::kinect_depth_compare_features.size();
    std::vector<sfr::KinectSeries> sb(features);
    for (size_t f = 0; f < features; ++f) sb[f] = series(b, f);
    std::vector<std::vector<double>> truth(features), other(features);
    std::vector<int64_t> pair_times;  // each pair's v1 image time
    uint64_t pairs = 0, lost_a = 0, lost_b = 0, lost_either = 0;
    double valid_a = 0, valid_b = 0, pixels_a = 0, pixels_b = 0;
    std::FILE* out = csv.empty() ? nullptr : std::fopen(csv.c_str(), "w");
    if (!csv.empty() && !out) std::cerr << "cannot write " << csv << '\n';
    if (out) {
        std::fprintf(out, "steady_us,v1_number,v2_steady_us");
        for (const auto& name : sfr::kinect_depth_compare_features) std::fprintf(out, ",v1_%s,v2_%s", name.c_str(), name.c_str());
        std::fprintf(out, "\n");
    }
    // The v2's presence of a player at a moment: both images around it have one.
    const sfr::KinectSeries b_pixels = series(b, 6);
    for (const auto& s : a.samples) {
        const int64_t at = s.steady_us + int64_t(std::llround(lag_us));
        if (at < b.samples.front().steady_us || at > b.samples.back().steady_us) continue;
        const double b_invalid = sfr::kinect_series_at(sb[18], at, gap);
        if (!std::isfinite(b_invalid)) continue;  // a gap in the v2's images
        ++pairs;
        pair_times.push_back(s.steady_us);
        valid_a += 1.0 - double(s.invalid);
        valid_b += 1.0 - b_invalid;
        const double b_player_pixels = sfr::kinect_series_at(b_pixels, at, gap);
        const bool b_player = std::isfinite(b_player_pixels);
        pixels_a += s.player ? double(s.features.centroid.pixels) : 0.0;
        pixels_b += b_player ? b_player_pixels : 0.0;
        lost_a += !s.player;
        lost_b += !b_player;
        lost_either += !s.player || !b_player;
        if (out) std::fprintf(out, "%lld,%llu,%lld", (long long)s.steady_us, (unsigned long long)s.number, (long long)at);
        for (size_t f = 0; f < features; ++f) {
            const double x = sfr::kinect_depth_feature(s, f), y = sfr::kinect_series_at(sb[f], at, gap);
            truth[f].push_back(x);
            other[f].push_back(y);
            if (out) {
                // Empty for none.
                const std::string cx = std::isfinite(x) ? format("%.9g", x) : "", cy = std::isfinite(y) ? format("%.9g", y) : "";
                std::fprintf(out, ",%s,%s", cx.c_str(), cy.c_str());
            }
        }
        if (out) std::fprintf(out, "\n");
    }
    const bool csv_written = out != nullptr;
    if (out) std::fclose(out);
    out = nullptr;
    if (!pairs) {
        line("沒有可以配對的影像（v2 在 v1 的每張影像附近都沒有影像）。");
        std::cout << text;
        return 1;
    }
    const auto percent = [](double value) { return format("%.1f%%", 100.0 * value); };
    line("配對影像 pairs: " + std::to_string(pairs) + " (v1 每張影像對 v2 同一時刻內插，間隔上限 " + format("%.0f", gap_ms) + " ms)");
    line("有效像素比例 valid ratio: v1 " + percent(valid_a / double(pairs)) + ", v2 " + percent(valid_b / double(pairs)));
    line("玩家像素 player pixels (平均): v1 " + format("%.0f", pixels_a / double(pairs)) + ", v2 " +
         format("%.0f", pixels_b / double(pairs)));
    line("失去玩家 lost: v1 " + percent(double(lost_a) / double(pairs)) + ", v2 " + percent(double(lost_b) / double(pairs)) +
         ", 任一台 either " + percent(double(lost_either) / double(pairs)));
    line("");
    line("特徵 feature            n      r       slope    intercept   rmse        resid_rmse  lag_ms   mean_v1     mean_v2");
    line("（v1 = slope * v2 + intercept；rmse 是 v1 - v2；lag_ms 是該特徵自己的最佳時間差，已扣掉上面的整體時間差）");
    std::vector<sfr::KinectFit> fits(features);
    for (size_t f = 0; f < features; ++f) {
        const sfr::KinectFit fit = fits[f] = sfr::kinect_fit(truth[f], other[f]);
        sfr::KinectSeries shifted = sb[f];
        for (auto& t : shifted.t) t -= int64_t(std::llround(lag_us));
        const sfr::KinectLag own = sfr::kinect_cross_correlate(series(a, f), shifted, max_lag, step);
        char row[512];
        std::snprintf(row, sizeof row, "%-20s %6zu %7s %10s %11s %11s %11s %8s %11s %11s",
                      sfr::kinect_depth_compare_features[f].c_str(), fit.n, format("%.3f", fit.r).c_str(),
                      format("%.4g", fit.slope).c_str(), format("%.4g", fit.intercept).c_str(),
                      format("%.4g", fit.rmse).c_str(), format("%.4g", fit.residual_rmse).c_str(),
                      own.found ? format("%+.0f", own.lag_us / 1000.0).c_str() : "-",
                      fit.n ? format("%.4g", fit.mean_truth).c_str() : "-", fit.n ? format("%.4g", fit.mean_other).c_str() : "-");
        line(row);
    }
    line("");
    line("說明：centroid/min/max 是像素欄列；count_* 是玩家在四個區塊的像素數（+640 下左 = 玩家右側，+644 下右）；"
         "mean_* 與 mean_depth 是 毫米<<3（除以 8 為毫米）；lean 是遊戲由 +640/+644 算出的傾斜；"
         "空的區塊與像素太少的影像不列入 mean_*。");
    line("");
    // Size and depth: the means over the pairs where both have a player.
    const double pixel_ratio = fits[6].n ? fits[6].mean_other / fits[6].mean_truth : NAN;
    const double depth_ratio = fits[17].n ? fits[17].mean_other / fits[17].mean_truth : NAN;
    line("輪廓大小比 player pixel ratio v2/v1: " + format("%.3f", pixel_ratio) + "；平均深度比 mean depth ratio v2/v1: " +
         format("%.3f", depth_ratio) + "（兩邊都有玩家的配對影像的平均）");
    std::string summary = "SUMMARY pairs=" + std::to_string(pairs);
    const auto add = [&](const std::string& key, double value) { summary += " " + key + "=" + format("%.4g", value); };
    add("lag_ms", lag_us / 1000.0);
    for (const auto& [name, f] : {std::pair<const char*, size_t>{"lean", 11}, {"centroid_row", 1}, {"mean_depth", 17}}) {
        add(std::string(name) + "_r", fits[f].r);
        add(std::string(name) + "_slope", fits[f].slope);
    }
    add("pixel_ratio", pixel_ratio);
    add("depth_ratio", depth_ratio);
    if (!events_path.empty()) {
        std::ifstream in(events_path);
        std::vector<sfr::KinectRaceRecord> rows;
        std::string text_line;
        while (std::getline(in, text_line)) {
            sfr::KinectRaceRecord row;
            if (sfr::kinect_parse_race_row(text_line, row)) rows.push_back(row);
        }
        if (!in.eof() || rows.empty()) {
            line("事件 events: 讀不到 " + events_path + "（或沒有任何列）");
        } else {
            line("遊戲判定的事件 events（" + events_path + "：crouch_original／jump_original 每段 1 的開頭；"
                 "基準 = 前 2.0–0.5 s 的平均，反應 = 之後 1.2 s 內的最大（蹲）／最小（跳）減基準，在對齊後的序列上）");
            line("  事件 event   特徵 feature      events measured   v1 平均     v2 平均     v2/v1    同方向 same");
            // crouch_original is detector 0, jump_original 2 (kinect_race_detectors).
            for (const auto& [event, detector, upward] :
                 {std::tuple<const char*, size_t, bool>{"crouch", 0, true}, {"jump", 2, false}}) {
                const std::vector<int64_t> starts = sfr::kinect_detector_event_starts(rows, detector);
                for (const auto& [feature, f] : {std::pair<const char*, size_t>{"centroid_row", 1}, {"min_row", 3}}) {
                    const sfr::KinectEventAmplitudes a =
                        sfr::kinect_event_amplitudes(pair_times, truth[f], other[f], starts, upward);
                    char row_text[256];
                    std::snprintf(row_text, sizeof row_text, "  %-11s %-16s %6zu %8zu %11s %11s %8s   %zu/%zu", event, feature,
                                  a.events, a.measured, format("%.1f", a.truth).c_str(), format("%.1f", a.other).c_str(),
                                  format("%.3f", a.ratio()).c_str(), a.same_direction, a.measured);
                    line(row_text);
                    const std::string key = std::string(event) + "_" + feature;
                    add(key + "_v1", a.truth);
                    add(key + "_v2", a.other);
                    add(key + "_ratio", a.ratio());
                    summary += " " + key + "_same=" + std::to_string(a.same_direction) + "/" + std::to_string(a.measured);
                }
            }
        }
    }
    if (csv_written) line("對齊後的序列 aligned series: " + csv);
    line(summary);
    std::cout << text;
    if (!report_path.empty()) {
        std::ofstream file(report_path, std::ios::binary);
        file << text;
        if (!file) std::cerr << "cannot write " << report_path << '\n';
    }
    return 0;
}
