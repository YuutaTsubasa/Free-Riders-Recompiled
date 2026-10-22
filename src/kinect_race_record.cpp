#include "kinect_race_record.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sfr {

const std::array<uint32_t, kinect_race_detector_count> kinect_race_detectors = {0x822C8778, 0x822CB840, 0x822C9050,
                                                                                 0x822CA6B0};
const std::array<const char*, kinect_race_detector_count> kinect_race_detector_names = {"crouch", "charge", "jump",
                                                                                        "side"};

int kinect_race_detector_index(uint32_t address) {
    for (uint32_t i = 0; i < kinect_race_detector_count; ++i)
        if (kinect_race_detectors[i] == address) return int(i);
    return -1;
}

namespace {
constexpr size_t race_columns = 6 + 2 * kinect_race_detector_count;
constexpr const char* view_columns[] = {
    "view_frame", "view_us", "slot", "centroid_col", "centroid_row", "min_col", "min_row", "max_col", "max_row",
    "count_lower_left", "count_lower_right", "count_upper_left", "count_upper_right",
    "mean_lower_left", "mean_lower_right", "mean_upper_left", "mean_upper_right", "near_count", "mean_depth",
};
constexpr size_t view_column_count = sizeof view_columns / sizeof view_columns[0];

void put_float(std::string& out, float value) {
    out += ',';
    if (!std::isfinite(value)) return;
    char text[32];
    std::snprintf(text, sizeof text, "%.9g", double(value));
    out += text;
}
void put_int(std::string& out, long long value) {
    out += ',';
    out += std::to_string(value);
}
bool number(std::string_view text, double& value) {
    if (text.empty()) return false;
    const std::string copy(text);
    char* end = nullptr;
    value = std::strtod(copy.c_str(), &end);
    return end && *end == 0;
}
bool whole(std::string_view text, long long& value) {
    if (text.empty()) return false;
    const std::string copy(text);
    char* end = nullptr;
    value = std::strtoll(copy.c_str(), &end, 10);
    return end && *end == 0;
}
}

std::string kinect_race_record_header() {
    std::string out = "steady_us,generation,record,tracking_id,depth_right,depth_left";
    for (const char* name : kinect_race_detector_names) {
        out += ',';
        out += name;
        out += "_original,";
        out += name;
        out += "_result";
    }
    for (const char* name : view_columns) {
        out += ',';
        out += name;
    }
    out += '\n';
    return out;
}

void kinect_race_record_row(const KinectRaceRecord& r, std::string& out) {
    out += std::to_string(r.steady_us);
    put_int(out, (long long)r.generation);
    put_int(out, r.record);
    put_int(out, r.tracking_id);
    put_float(out, r.depth_right);
    put_float(out, r.depth_left);
    for (uint32_t i = 0; i < kinect_race_detector_count; ++i) {
        if (r.original[i] >= 0) put_int(out, r.original[i]); else out += ',';
        if (r.result[i] >= 0) put_int(out, r.result[i]); else out += ',';
    }
    const KinectDepthViewRecord& v = r.view;
    if (!v.known) {
        out.append(view_column_count, ',');
    } else {
        put_int(out, (long long)v.frame);
        put_int(out, v.steady_us);
        put_int(out, v.slot);
        for (uint32_t value : {v.centroid.column, v.centroid.row, v.centroid.min_column, v.centroid.min_row,
                               v.centroid.max_column, v.centroid.max_row})
            put_int(out, value);
        for (float value : v.counts) put_float(out, value);
        for (float value : v.means) put_float(out, value);
        put_int(out, v.near_count);
        put_float(out, v.average);
    }
    out += '\n';
}

bool kinect_parse_race_row(std::string_view line, KinectRaceRecord& r) {
    const auto f = kinect_record_fields(line);
    // Files from before the depth view's columns have only the first ones.
    if (f.size() != race_columns && f.size() != race_columns + view_column_count) return false;
    double v = 0;
    r = {};
    if (!number(f[0], v)) return false;  // the header
    r.steady_us = int64_t(v);
    size_t at = 1;
    const auto next = [&](double fallback = 0) { return number(f[at++], v) ? v : fallback; };
    r.generation = uint64_t(next());
    r.record = uint32_t(next());
    r.tracking_id = uint32_t(next());
    r.depth_right = float(next());
    r.depth_left = float(next());
    for (uint32_t i = 0; i < kinect_race_detector_count; ++i) {
        r.original[i] = int(next(-1));
        r.result[i] = int(next(-1));
    }
    if (f.size() == race_columns) return true;
    KinectDepthViewRecord& view = r.view;
    long long value = 0;
    if (!whole(f[at], value)) return true;  // no depth view yet for this record
    view.known = true;
    const auto integer = [&]() { return whole(f[at++], value) ? value : 0; };
    view.frame = uint64_t(integer());
    view.steady_us = int64_t(integer());
    view.slot = uint32_t(integer());
    view.centroid.column = uint32_t(integer());
    view.centroid.row = uint32_t(integer());
    view.centroid.min_column = uint32_t(integer());
    view.centroid.min_row = uint32_t(integer());
    view.centroid.max_column = uint32_t(integer());
    view.centroid.max_row = uint32_t(integer());
    for (float& count : view.counts) count = float(next(NAN));
    for (float& mean : view.means) mean = float(next(NAN));
    view.near_count = uint32_t(integer());
    view.average = float(next(NAN));
    return true;
}

KinectRecordFile* kinect_record_race() {
    static std::mutex lock;
    static std::unique_ptr<KinectRecordFile> file;
    std::lock_guard guard(lock);
    const std::string& prefix = kinect_record_prefix();
    if (prefix.empty()) return nullptr;
    if (!file)
        file = std::make_unique<KinectRecordFile>(prefix + "-race.csv", kinect_race_record_header(),
                                                  kinect_record_limit_bytes());
    return file.get();
}

namespace {
std::mutex& view_lock() {
    static std::mutex lock;
    return lock;
}
std::unordered_map<uint32_t, KinectDepthViewRecord>& views() {
    static std::unordered_map<uint32_t, KinectDepthViewRecord> table;
    return table;
}
}

void kinect_depth_view_publish(uint32_t record, const KinectDepthViewRecord& view) {
    std::lock_guard guard(view_lock());
    views()[record] = view;
}

bool kinect_depth_view_latest(uint32_t record, KinectDepthViewRecord& view) {
    std::lock_guard guard(view_lock());
    const auto found = views().find(record);
    if (found == views().end()) return false;
    view = found->second;
    return true;
}

}
