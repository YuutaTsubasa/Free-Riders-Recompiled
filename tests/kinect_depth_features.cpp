// The title's depth view, again, over a depth recording (kinect_record_depth.h,
// kinect_depth_features.h): per image and per player the numbers the game's
// own depth detectors read, without the game.
//
//   sfr_kinect_depth_features <depth.bin> [--csv out.csv] [--slot N] [--summary]
//                             [--validate <prefix>-race.csv] [--show N]
//
// --csv writes one row per image and player slot with pixels (slot empty
// when the image has no player): the image's clocks and number, its share of
// pixels without a depth, its player pixels, then pass one (centroid,
// extents, the player's pixel count) and pass two (the four parts' counts,
// whether the rest was written, their mean depths in millimetres << 3, the
// near count and the average). --slot N only that skeleton slot (0..5, every
// image). --summary prints the file's images, span, rate, mean share
// without a depth and player pixels (also printed after --csv).
//
// --validate compares, for a recording the game made (SFR_KINECT_RECORD with
// SFR_KINECT_RECORD_DEPTH=1, so its <prefix>-depth.bin and <prefix>-race.csv
// are of the same images), what the game's depth view wrote for each image
// (the race CSV's view_frame and following columns) with what this tool
// computes for the image of that number and the same slot: matched by the
// image's number, which both files take from the same counter (the number
// the game sees in NUI_IMAGE_FRAME +8), so no clock is involved. A race row
// is written every game update and the view runs on every new image (the
// title asks for one every fourth frame), so rows repeat an image: each
// (image, slot) is checked once. It prints the exact-match rate field by
// field and overall, and the first --show N (default 10) differences.
// Exit status 0 when every compared field matched, 3 otherwise.
#include "kinect_depth_compare.h"
#include "kinect_depth_features.h"
#include "kinect_race_record.h"
#include "kinect_record_depth.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
void put_features(std::FILE* out, const sfr::KinectDepthFeatures& f) {
    const auto& c = f.centroid;
    const auto& s = f.split;
    std::fprintf(out, ",%u,%u,%u,%u,%u,%u,%u,%u", f.slot, c.pixels, c.column, c.row, c.min_column, c.min_row,
                 c.max_column, c.max_row);
    for (float v : s.counts) std::fprintf(out, ",%.9g", double(v));
    std::fprintf(out, ",%d", s.written ? 1 : 0);
    if (s.written) {
        for (float v : s.means) std::fprintf(out, ",%.9g", double(v));
        std::fprintf(out, ",%u,%.9g", s.near_count, double(s.average));
    } else {
        std::fprintf(out, ",,,,,,");
    }
}

int validate(const std::string& depth_path, const std::string& race_path, size_t show) {
    // The game's own values, once per (image, slot).
    std::ifstream in(race_path);
    if (!in) {
        std::cerr << "cannot read " << race_path << '\n';
        return 1;
    }
    std::map<std::pair<uint64_t, uint32_t>, sfr::KinectDepthViewRecord> game;
    uint64_t rows = 0, view_rows = 0, inconsistent = 0;
    std::string line;
    while (std::getline(in, line)) {
        sfr::KinectRaceRecord row;
        if (!sfr::kinect_parse_race_row(line, row)) continue;
        ++rows;
        if (!row.view.known) continue;
        ++view_rows;
        const auto key = std::make_pair(row.view.frame, row.view.slot);
        const auto [at, added] = game.emplace(key, row.view);
        // The same image's values must be the same in every row.
        if (!added && (at->second.counts != row.view.counts || at->second.centroid.column != row.view.centroid.column ||
                       at->second.near_count != row.view.near_count))
            ++inconsistent;
    }
    if (game.empty()) {
        std::cerr << race_path << ": " << rows << " race rows, none with the depth view's columns "
                  << "(a recording from before them, or no race with a real Kinect)\n";
        return 1;
    }
    sfr::KinectDepthReader reader;
    if (!reader.open(depth_path)) {
        std::cerr << depth_path << ": " << reader.error() << '\n';
        return 1;
    }
    // One pass over the images (a long recording does not fit in memory):
    // each image the game's rows name is checked as it is read.
    std::array<uint64_t, sfr::kinect_depth_view_field_count> compared{}, equal{};
    uint64_t checked = 0, all = 0, not_written = 0, images = 0;
    std::set<std::pair<uint64_t, uint32_t>> seen;
    size_t shown = 0;
    sfr::KinectDepthRecordFrame image;
    while (reader.next(image)) {
        ++images;
        for (auto at = game.lower_bound({image.number, 0}); at != game.end() && at->first.first == image.number; ++at) {
            const auto& [key, view] = *at;
            if (!seen.insert(key).second) continue;  // an image number twice: the first is the game's
            const sfr::KinectDepthFeatures tool =
                sfr::kinect_depth_view_features(image.pixels.data(), image.width, image.height, key.second);
            const sfr::KinectDepthViewCheck check = sfr::kinect_depth_view_check(view, tool);
            ++checked;
            if (!tool.split.written) ++not_written;
            for (size_t i = 0; i < sfr::kinect_depth_view_field_count; ++i) {
                compared[i] += check.compared[i];
                equal[i] += check.compared[i] && check.equal[i];
            }
            if (check.all_equal()) {
                ++all;
                continue;
            }
            if (shown++ >= show) continue;
            std::printf("  image %llu slot %u differs:", (unsigned long long)key.first, key.second);
            for (size_t i = 0; i < sfr::kinect_depth_view_field_count; ++i)
                if (check.compared[i] && !check.equal[i])
                    std::printf(" %s game=%s tool=%s", sfr::kinect_depth_view_fields[i], check.game[i].c_str(),
                                check.tool[i].c_str());
            std::printf("\n");
        }
    }
    const uint64_t missing = game.size() - checked;
    std::printf("validate: %s against %s\n", depth_path.c_str(), race_path.c_str());
    std::printf("  race rows %llu, with the depth view %llu; distinct (image, slot) %zu; depth images in the file %llu%s\n",
                (unsigned long long)rows, (unsigned long long)view_rows, game.size(), (unsigned long long)images,
                reader.truncated() ? " (the last cut short)" : "");
    std::printf("  matched by image number %llu; not in the depth file %llu (SFR_KINECT_RECORD_DEPTH_EVERY, its cap, "
                "or images before it opened)\n",
                (unsigned long long)checked, (unsigned long long)missing);
    if (inconsistent) std::printf("  rows of one image disagreeing among themselves: %llu\n", (unsigned long long)inconsistent);
    std::printf("  images too sparse for more than the zeroed counts: %llu (means, near, average not compared there)\n",
                (unsigned long long)not_written);
    for (size_t i = 0; i < sfr::kinect_depth_view_field_count; ++i)
        if (compared[i])
            std::printf("  %-18s %llu/%llu exact (%.2f%%)\n", sfr::kinect_depth_view_fields[i],
                        (unsigned long long)equal[i], (unsigned long long)compared[i],
                        100.0 * double(equal[i]) / double(compared[i]));
    if (checked)
        std::printf("  all fields exact: %llu/%llu (%.2f%%)\n", (unsigned long long)all, (unsigned long long)checked,
                    100.0 * double(all) / double(checked));
    return checked && all == checked ? 0 : 3;
}
}

int main(int argc, char** argv) {
    std::string path, csv, race;
    int only = -1;
    bool summary = false;
    size_t show = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--csv" && i + 1 < argc) csv = argv[++i];
        else if (arg == "--slot" && i + 1 < argc) only = std::atoi(argv[++i]);
        else if (arg == "--summary") summary = true;
        else if (arg == "--validate" && i + 1 < argc) race = argv[++i];
        else if (arg == "--show" && i + 1 < argc) show = size_t(std::atoi(argv[++i]));
        else if (path.empty()) path = arg;
        else {
            std::cerr << "unknown argument " << arg << '\n';
            return 2;
        }
    }
    if (path.empty() || (only > 5)) {
        std::cerr << "usage: sfr_kinect_depth_features <depth.bin> [--csv out.csv] [--slot 0..5] [--summary]\n"
                     "                                 [--validate <prefix>-race.csv] [--show N]\n";
        return 2;
    }
    if (!race.empty()) return validate(path, race, show);
    sfr::KinectDepthReader reader;
    if (!reader.open(path)) {
        std::cerr << path << ": " << reader.error() << '\n';
        return 1;
    }
    std::FILE* out = nullptr;
    if (!csv.empty()) {
        out = std::fopen(csv.c_str(), "w");
        if (!out) {
            std::cerr << "cannot write " << csv << '\n';
            return 1;
        }
        std::fprintf(out, "number,steady_us,handed_us,sdk_time,sdk_frame,source,invalid_ratio,player_pixels,slot,pixels,"
                          "centroid_col,centroid_row,min_col,min_row,max_col,max_row,count_lower_left,count_lower_right,"
                          "count_upper_left,count_upper_right,written,mean_lower_left,mean_lower_right,mean_upper_left,"
                          "mean_upper_right,near_count,mean_depth\n");
    }
    sfr::KinectDepthRecordFrame frame;
    uint64_t images = 0, with_player = 0;
    double invalid = 0, player_pixels = 0;
    int64_t first = 0, last = 0;
    std::string source;
    while (reader.next(frame)) {
        if (!images) first = frame.steady_us;
        last = frame.steady_us;
        ++images;
        source = sfr::kinect_depth_source_name(frame.source);
        const sfr::KinectDepthImageStats stats = sfr::kinect_depth_image_stats(frame.pixels.data(), frame.pixels.size());
        uint32_t players = 0;
        for (uint32_t index = 1; index <= 6; ++index) players += stats.players[index];
        invalid += stats.invalid;
        player_pixels += players;
        with_player += players > 0;
        if (!out) continue;
        const auto prefix = [&] {
            std::fprintf(out, "%llu,%lld,%lld,%lld,%llu,%s,%.6f,%u", (unsigned long long)frame.number,
                         (long long)frame.steady_us, (long long)frame.handed_us, (long long)frame.sdk_time,
                         (unsigned long long)frame.sdk_frame, source.c_str(), double(stats.invalid), players);
        };
        bool any = false;
        for (uint32_t slot = 0; slot < 6; ++slot) {
            if (only >= 0 ? int(slot) != only : !stats.players[slot + 1]) continue;
            any = true;
            prefix();
            put_features(out, sfr::kinect_depth_view_features(frame.pixels.data(), frame.width, frame.height, slot));
            std::fprintf(out, "\n");
        }
        if (!any) {
            prefix();
            std::fprintf(out, "%s\n", std::string(19, ',').c_str());  // no player: the slot's columns empty
        }
    }
    if (out) std::fclose(out);
    if (!reader.error().empty()) std::cerr << path << ": stopped at " << reader.error() << '\n';
    // The summary, always (--summary alone writes nothing else).
    (void)summary;
    const double seconds = images > 1 ? double(last - first) / 1e6 : 0;
    std::printf("%s: %s, %ux%u, %llu images over %.1f s (%.1f a second)%s\n", path.c_str(), source.c_str(),
                reader.width(), reader.height(), (unsigned long long)images, seconds,
                seconds > 0 ? double(images - 1) / seconds : 0.0, reader.truncated() ? ", the last cut short" : "");
    if (images)
        std::printf("  invalid_ratio %.2f%%, player pixels %.0f on average, images with a player %.1f%%\n",
                    100.0 * invalid / double(images), player_pixels / double(images),
                    100.0 * double(with_player) / double(images));
    if (out) std::printf("  features written to %s\n", csv.c_str());
    return images ? 0 : 1;
}
