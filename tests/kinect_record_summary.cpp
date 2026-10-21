// Compares two Kinect recordings of a player standing still
// (kinect_record.h): one with a sensor alone, one with both sensors on, to
// see whether the other sensor's light makes it worse.
//
//   sfr_kinect_record_summary <alone>-skeleton.csv <both>-skeleton.csv [--sensor v1|v2]
//
// For each sensor in both files (or the one named) it prints, alone and with
// both: joint jitter (SpineBase, SpineShoulder, both hands; millimetres RMS),
// the share of joints tracked, the share of depth pixels without a depth and
// how often the player was lost, then checks them against the plan's limits:
// jitter up by less than 20%, tracked down by less than 5 points, depth
// holes up by less than 10 points, and the player never lost.
#include "kinect_record.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
bool load(const std::string& path, std::vector<sfr::KinectRecordRow>& rows, std::set<std::string>& sensors) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        sfr::KinectRecordRow row;
        if (!sfr::kinect_parse_skeleton_row(line, row)) continue;
        sensors.insert(row.frame.sensor);
        rows.push_back(std::move(row));
    }
    return true;
}

void print(const char* label, const sfr::KinectRecordStats& s) {
    std::printf("  %-6s frames=%llu with_body=%llu jitter_mm=", label, (unsigned long long)s.frames,
                (unsigned long long)s.with_body);
    for (size_t k = 0; k < s.jitter.size(); ++k)
        std::printf("%s%.2f", k ? "/" : "", double(s.jitter[k]) * 1000.0);
    std::printf(" tracked=%.1f%%", double(s.tracked) * 100.0);
    if (s.depth_invalid >= 0) std::printf(" depth_invalid=%.1f%%", double(s.depth_invalid) * 100.0);
    else std::printf(" depth_invalid=none");
    std::printf(" lost=%u clipped_frames=%llu\n", s.lost, (unsigned long long)s.clipped);
}
}

int main(int argc, char** argv) {
    std::vector<std::string> paths;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sensor" && i + 1 < argc) only = argv[++i];
        else paths.push_back(arg);
    }
    if (paths.size() != 2) {
        std::cerr << "usage: sfr_kinect_record_summary <alone.csv> <both.csv> [--sensor v1|v2]\n";
        return 2;
    }
    std::vector<sfr::KinectRecordRow> alone, both;
    std::set<std::string> alone_sensors, both_sensors;
    if (!load(paths[0], alone, alone_sensors) || !load(paths[1], both, both_sensors)) {
        std::cerr << "cannot read the recordings\n";
        return 1;
    }
    bool all_pass = true, any = false;
    for (const std::string& sensor : alone_sensors) {
        if (!only.empty() && sensor != only) continue;
        if (!both_sensors.count(sensor)) continue;
        any = true;
        const auto a = sfr::kinect_record_stats(alone, sensor), b = sfr::kinect_record_stats(both, sensor);
        std::printf("Kinect %s (jitter: spine_base/spine_shoulder/hand_left/hand_right)\n", sensor.c_str());
        print("alone", a);
        print("both", b);
        bool pass = true;
        if (!a.with_body || !b.with_body) {
            // Nothing to compare: not a pass.
            std::printf("  nobody tracked in %s: nothing measured\n  FAIL\n", !a.with_body ? "the first" : "the second");
            all_pass = false;
            continue;
        }
        const char* names[4] = {"spine_base", "spine_shoulder", "hand_left", "hand_right"};
        for (size_t k = 0; k < 4; ++k) {
            if (!std::isfinite(a.jitter[k]) || !std::isfinite(b.jitter[k]) || !(a.jitter[k] > 0)) {
                std::printf("  jitter %-14s not measured\n", names[k]);
                continue;
            }
            const double rise = (double(b.jitter[k]) / double(a.jitter[k]) - 1.0) * 100.0;
            const bool ok = rise < 20.0;
            pass = pass && ok;
            std::printf("  jitter %-14s %+6.1f%%  (limit +20%%)  %s\n", names[k], rise, ok ? "ok" : "FAIL");
        }
        const double drop = (double(a.tracked) - double(b.tracked)) * 100.0;
        const bool tracked_ok = drop < 5.0;
        std::printf("  tracked drop          %+6.1f points (limit 5)  %s\n", drop, tracked_ok ? "ok" : "FAIL");
        pass = pass && tracked_ok;
        if (a.depth_invalid >= 0 && b.depth_invalid >= 0) {
            const double rise = (double(b.depth_invalid) - double(a.depth_invalid)) * 100.0;
            const bool ok = rise < 10.0;
            std::printf("  depth holes rise      %+6.1f points (limit 10)  %s\n", rise, ok ? "ok" : "FAIL");
            pass = pass && ok;
        } else {
            std::printf("  depth holes           not recorded\n");
        }
        const bool kept = b.lost == 0;
        std::printf("  lost with both        %u (limit 0)  %s\n", b.lost, kept ? "ok" : "FAIL");
        pass = pass && kept;
        std::printf("  %s\n", pass ? "PASS" : "FAIL");
        all_pass = all_pass && pass;
    }
    if (!any) {
        std::cerr << "no sensor is in both recordings\n";
        return 1;
    }
    return all_pass ? 0 : 3;
}
