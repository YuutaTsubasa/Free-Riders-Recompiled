#pragma once
#include "kinect_depth_features.h"
#include "kinect_record.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace sfr {

// The race's side of a recording (kinect_record.h): with SFR_KINECT_RECORD
// set, <prefix>-race.csv gets one row per game update per player record
// that a real sensor drives: the title's own depth view's lean pair (record
// +640, +644) as the title computed it, and what the title's own crouch,
// charge, jump and side detectors answered on that update (original) and
// what the race used (result: the same, nothing stands in for a real
// Kinect's depth). With a Kinect v1 these are the console's own depth-based
// gestures, with a Kinect v2 the same detectors over the depth the v2 makes
// (kinect_v2_depth.h), so a v1 session and a v2 session can be compared.
// After those columns, everything the depth view computed for the player
// from its newest image (KinectDepthViewRecord): the image's number, so the
// image itself (kinect_record_depth.h) can be matched and its features
// computed again offline (sfr_kinect_depth_features --validate).

// The title's detectors a row records, in its column order.
constexpr uint32_t kinect_race_detector_count = 4;
extern const std::array<uint32_t, kinect_race_detector_count> kinect_race_detectors;  // 822C8778 ...
extern const std::array<const char*, kinect_race_detector_count> kinect_race_detector_names;
// The column of a detector, or -1 when a row does not record it.
int kinect_race_detector_index(uint32_t address);

// Everything the title's depth view wrote for one player from one depth
// image (kinect_depth_features.h has the layout and the code it comes
// from), read from guest memory right after the view ran on that image
// (nui_hooks.cpp, 82439530), so all of it is of the same image.
struct KinectDepthViewRecord {
    bool known = false;       // false: no depth view has run for this record yet
    uint64_t frame = 0;       // the depth image's number (NUI_IMAGE_FRAME +8, KinectImage::number)
    int64_t steady_us = 0;    // when the view finished with it
    uint32_t slot = 0;        // the skeleton slot (player index slot + 1)
    KinectDepthCentroid centroid;  // manager+7584/7632/7680 + 8*slot (pixels: not kept, 0)
    // record +640..+679 as they are in memory: when the image had too few of
    // the player's pixels the counts are zero and the rest an older image's.
    std::array<float, 4> counts{}, means{};
    uint32_t near_count = 0;
    float average = 0;
};

struct KinectRaceRecord {
    int64_t steady_us = 0;
    uint64_t generation = 0;   // the sensor frame the update read
    uint32_t record = 0;       // the title's body record (guest address)
    uint32_t tracking_id = 0;  // its skeleton's tracking id
    // The title's depth view's lean pair.
    float depth_right = 0, depth_left = 0;
    // Each detector's answer on this update: the title's own (original) and
    // the one the race used (result); -1 when it was not asked.
    std::array<int, kinect_race_detector_count> original{-1, -1, -1, -1}, result{-1, -1, -1, -1};
    // The depth view's newest features for this record (columns after the
    // detectors; a file from before them still loads, with known false).
    KinectDepthViewRecord view;
};

std::string kinect_race_record_header();
void kinect_race_record_row(const KinectRaceRecord& row, std::string& out);
bool kinect_parse_race_row(std::string_view line, KinectRaceRecord& row);

// The race file, or null when recording is off.
KinectRecordFile* kinect_record_race();

// The depth view's features of a body record, kept from the view's thread
// (kinect_depth_view_publish) for the race's rows (kinect_depth_view_latest).
void kinect_depth_view_publish(uint32_t record, const KinectDepthViewRecord& view);
bool kinect_depth_view_latest(uint32_t record, KinectDepthViewRecord& view);

}
