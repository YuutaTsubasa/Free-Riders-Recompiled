#pragma once
#include "kinect_record.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace sfr {

// The title's side of a recording (kinect_record.h), past what race.csv
// holds: with SFR_KINECT_RECORD set and a real Kinect playing,
// <prefix>-nui.csv gets one row per input update (824920F8) per player with
// a body record: what the title's own consumers of the skeleton made of it
// on that update, for comparing a v1 session with a v2 one and for every
// later change (NUI_SKELETON_ANALYSIS.md, S2).
//
// - The menu hand cursor of each hand as 82492F30 left it (packet P =
//   input + 32 + 2544 player + 16 + 400 hand): flags (P+0), theta (P+60),
//   r (P+148), the target (P+96/+100) and the cursor (P+80/+84, pixels).
// - Joints of the player's body record (input block +12) as the title read
//   them, after its smoothing and in its mirrored space (-x, y, -z):
//   hands, shoulders, shoulder centre, head, spine and hip centre.
// - The race's lean pair from 822C6200 (the means of its two filters,
//   source +44 from the depth, +88 from the body's tilt).
// - Every race detector the study lists (kinect_nui_detectors), asked since
//   the player's last row: its answer, the bits it added to its result
//   entry's +4 (primary) and +20 (secondary) words and its own value where
//   it has one (see kinect_nui_detectors); empty when it was not asked.
// - When a race preparation measurement completed since the last row
//   (822C65B0 arms forward, 822C6AD8 the body), its step and the
//   calibration object's +0..+132 (+112..+120 whole numbers, the rest
//   floats).
// Bounded by SFR_KINECT_RECORD_MB and written through the record queue
// like the other files, so the game's thread never waits for the disk.

struct KinectNuiDetector {
    uint32_t address;
    const char* name;
    // Where its value is: 0 none; else as kinect_nui_value_kind.
    uint8_t value;
};
namespace kinect_nui_value_kind {
constexpr uint8_t none = 0, entry_byte_80 = 1, entry_word_72 = 2, entry_float_56 = 3, entry_signed_82 = 4,
                  entry_signed_83 = 5, second_entry_signed_81 = 6, detector_short_76 = 7, entry_signed_76 = 8;
}
constexpr uint32_t kinect_nui_detector_count = 32;
extern const std::array<KinectNuiDetector, kinect_nui_detector_count> kinect_nui_detectors;
int kinect_nui_detector_index(uint32_t address);

struct KinectNuiDetectorAnswer {
    int result = -1;              // -1: not asked
    uint32_t primary = 0, secondary = 0;
    bool has_value = false;
    float value = 0;
    uint32_t calls = 0;
};
struct KinectNuiHand {
    uint32_t flags = 0;
    float theta = 0, r = 0, target_x = 0, target_y = 0, cursor_x = 0, cursor_y = 0;
};
constexpr uint32_t kinect_nui_joint_count = 8;
extern const std::array<uint32_t, kinect_nui_joint_count> kinect_nui_joints;  // nui_joint numbers
extern const std::array<const char*, kinect_nui_joint_count> kinect_nui_joint_names;
constexpr uint32_t kinect_nui_calibration_words = 34;  // +0..+132

struct KinectNuiRecord {
    int64_t steady_us = 0;
    uint64_t update = 0;      // the input update's count
    uint32_t player = 0;
    uint32_t tracking_id = 0;
    int32_t body_slot = -1;   // the record's skeleton slot (+784)
    bool racing = false;
    bool has_cursor = false;
    std::array<KinectNuiHand, 2> hands{};  // left, right
    bool has_joints = false;
    std::array<std::array<float, 3>, kinect_nui_joint_count> joints{};
    bool has_lean = false;
    float lean_depth = 0, lean_tilt = 0;
    std::array<KinectNuiDetectorAnswer, kinect_nui_detector_count> detectors{};
    bool has_wipe_y = false;
    float wipe_y = 0;          // 822CDEE0's second coordinate (detector +78)
    std::string calibration_step;  // empty: none completed
    std::array<uint32_t, kinect_nui_calibration_words> calibration{};
};

std::string kinect_nui_record_header();
void kinect_nui_record_row(const KinectNuiRecord& row, std::string& out);
bool kinect_parse_nui_row(std::string_view line, KinectNuiRecord& row);

// The file, or null when recording is off.
KinectRecordFile* kinect_record_nui();

// What the race and its preparation saw of each player between two input
// updates, gathered from their hooks (any thread) and taken into the
// player's next row.
class KinectNuiRecorder {
public:
    static constexpr uint32_t players = 2;
    void detector(uint32_t player, uint32_t address, int result, uint32_t primary, uint32_t secondary, bool has_value,
                  float value);
    void wipe_y(uint32_t player, float value);
    void lean(uint32_t player, float depth, float tilt);
    void calibration(uint32_t player, const char* step, const std::array<uint32_t, kinect_nui_calibration_words>& words);
    // Moves what was gathered for the player into row (detectors, lean,
    // calibration) and starts the player's next one empty.
    void take(uint32_t player, KinectNuiRecord& row);
private:
    std::mutex lock_;
    std::array<KinectNuiRecord, players> pending_{};
};
KinectNuiRecorder& kinect_nui_recorder();

}
