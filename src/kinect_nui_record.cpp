#include "kinect_nui_record.h"
#include "nui_skeleton.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace sfr {

namespace v = kinect_nui_value_kind;
// The race detectors of NUI_SKELETON_ANALYSIS.md (section 3.3 and appendix
// A), by their original functions, with the value each keeps besides its
// bits: brake and grab strength (entry +80, percent), the stance (entry
// +72), the trick's turn (entry +56, degrees), the lever and handle (entry
// +82/+83, signed percent), the front curve (the second entry's +81), the
// steam wipe's x (detector +76; its y is wipe_y) and the steering (entry +76,
// signed).
const std::array<KinectNuiDetector, kinect_nui_detector_count> kinect_nui_detectors = {{
    {0x822C9050, "jump", v::none},           {0x822C8778, "crouch", v::none},
    {0x822CB840, "charge", v::none},         {0x822C8650, "arms_up", v::none},
    {0x822C9E90, "catch_lr", v::none},       {0x822CB340, "catch", v::none},
    {0x822C9180, "flying", v::none},         {0x822C9A80, "accel", v::none},
    {0x822CAF48, "accel2", v::none},         {0x822CA6B0, "side", v::none},
    {0x822C9BF0, "brake", v::entry_byte_80}, {0x822CB0B8, "grab", v::entry_byte_80},
    {0x822CA518, "kick", v::none},           {0x822CBD28, "kick2", v::none},
    {0x822CA810, "power", v::none},          {0x822CBF30, "power2", v::none},
    {0x822CAC90, "stance", v::entry_word_72}, {0x822C9938, "trick", v::entry_float_56},
    {0x822CC2D0, "hammer", v::none},         {0x822CC590, "over_throw", v::none},
    {0x822CCD98, "under_swing", v::none},    {0x822CD068, "under_throw", v::none},
    {0x822CDDB0, "inflator", v::none},       {0x822CC8F0, "shake", v::none},
    {0x822CDEE0, "wipe", v::detector_short_76}, {0x822CD638, "lever", v::entry_signed_82},
    {0x822CD960, "handle", v::entry_signed_83}, {0x822CD3F0, "front_curve", v::second_entry_signed_81},
    {0x822CE118, "swim", v::none},           {0x822C8D88, "hold_hands", v::none},
    {0x822C8958, "steer", v::entry_signed_76}, {0x822CB9B8, "steer_alt", v::entry_signed_76},
}};

int kinect_nui_detector_index(uint32_t address) {
    for (uint32_t i = 0; i < kinect_nui_detector_count; ++i)
        if (kinect_nui_detectors[i].address == address) return int(i);
    return -1;
}

const std::array<uint32_t, kinect_nui_joint_count> kinect_nui_joints = {
    nui_joint::hand_left, nui_joint::hand_right, nui_joint::shoulder_left, nui_joint::shoulder_right,
    nui_joint::shoulder_center, nui_joint::head, nui_joint::spine, nui_joint::hip_center};
const std::array<const char*, kinect_nui_joint_count> kinect_nui_joint_names = {
    "hand_l", "hand_r", "shoulder_l", "shoulder_r", "shoulder_c", "head", "spine", "hip_c"};

namespace {
constexpr const char* hand_fields[] = {"flags", "theta", "r", "target_x", "target_y", "cursor_x", "cursor_y"};
constexpr const char* hand_names[] = {"left", "right"};
constexpr const char* detector_fields[] = {"result", "primary", "secondary", "value"};
constexpr size_t fixed_columns = 6;
constexpr size_t column_count = fixed_columns + 2 * 7 + 3 * kinect_nui_joint_count + 2 + 4 * kinect_nui_detector_count + 1 +
                                1 + kinect_nui_calibration_words;
// The calibration object's whole-number words (pixels).
bool calibration_integer(uint32_t index) { return index * 4 >= 112 && index * 4 <= 120; }

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
}

std::string kinect_nui_record_header() {
    std::string out = "steady_us,update,player,tracking_id,body_slot,racing";
    for (const char* hand : hand_names)
        for (const char* field : hand_fields) out.append(",").append(hand).append("_").append(field);
    for (const char* joint : kinect_nui_joint_names)
        for (const char* axis : {"x", "y", "z"}) out.append(",").append(joint).append("_").append(axis);
    out += ",lean_depth,lean_tilt";
    for (const KinectNuiDetector& detector : kinect_nui_detectors)
        for (const char* field : detector_fields) out.append(",").append(detector.name).append("_").append(field);
    out += ",wipe_y,calib_step";
    for (uint32_t i = 0; i < kinect_nui_calibration_words; ++i) out += ",calib_" + std::to_string(4 * i);
    out += '\n';
    return out;
}

void kinect_nui_record_row(const KinectNuiRecord& r, std::string& out) {
    out += std::to_string(r.steady_us);
    put_int(out, (long long)r.update);
    put_int(out, r.player);
    put_int(out, r.tracking_id);
    put_int(out, r.body_slot);
    put_int(out, r.racing ? 1 : 0);
    for (const KinectNuiHand& hand : r.hands) {
        if (!r.has_cursor) { out.append(7, ','); continue; }
        put_int(out, hand.flags);
        for (float value : {hand.theta, hand.r, hand.target_x, hand.target_y, hand.cursor_x, hand.cursor_y}) put_float(out, value);
    }
    for (const auto& joint : r.joints) {
        if (!r.has_joints) { out.append(3, ','); continue; }
        for (float value : joint) put_float(out, value);
    }
    if (r.has_lean) { put_float(out, r.lean_depth); put_float(out, r.lean_tilt); }
    else out.append(2, ',');
    for (const KinectNuiDetectorAnswer& answer : r.detectors) {
        if (answer.result < 0) { out.append(4, ','); continue; }
        put_int(out, answer.result);
        put_int(out, answer.primary);
        put_int(out, answer.secondary);
        if (answer.has_value) put_float(out, answer.value); else out += ',';
    }
    if (r.has_wipe_y) put_float(out, r.wipe_y); else out += ',';
    out += ',';
    out += r.calibration_step;
    for (uint32_t i = 0; i < kinect_nui_calibration_words; ++i) {
        if (r.calibration_step.empty()) { out += ','; continue; }
        if (calibration_integer(i)) put_int(out, int32_t(r.calibration[i]));
        else put_float(out, std::bit_cast<float>(r.calibration[i]));
    }
    out += '\n';
}

bool kinect_parse_nui_row(std::string_view line, KinectNuiRecord& r) {
    const auto f = kinect_record_fields(line);
    if (f.size() != column_count) return false;
    double v = 0;
    r = {};
    if (!number(f[0], v)) return false;  // the header
    r.steady_us = int64_t(v);
    size_t at = 1;
    const auto next = [&](double fallback = 0) { return number(f[at++], v) ? v : fallback; };
    const auto present = [&](size_t count) {
        for (size_t i = 0; i < count; ++i)
            if (!f[at + i].empty()) return true;
        return false;
    };
    r.update = uint64_t(next());
    r.player = uint32_t(next());
    r.tracking_id = uint32_t(next());
    r.body_slot = int32_t(next(-1));
    r.racing = next() != 0;
    r.has_cursor = present(14);
    for (KinectNuiHand& hand : r.hands) {
        hand.flags = uint32_t(next());
        hand.theta = float(next());
        hand.r = float(next());
        hand.target_x = float(next());
        hand.target_y = float(next());
        hand.cursor_x = float(next());
        hand.cursor_y = float(next());
    }
    r.has_joints = present(3 * kinect_nui_joint_count);
    for (auto& joint : r.joints)
        for (float& value : joint) value = float(next());
    r.has_lean = present(2);
    r.lean_depth = float(next());
    r.lean_tilt = float(next());
    for (KinectNuiDetectorAnswer& answer : r.detectors) {
        answer.result = int(next(-1));
        answer.primary = uint32_t(next());
        answer.secondary = uint32_t(next());
        answer.has_value = !f[at].empty();
        answer.value = float(next());
    }
    r.has_wipe_y = !f[at].empty();
    r.wipe_y = float(next());
    r.calibration_step = std::string(f[at++]);
    for (uint32_t i = 0; i < kinect_nui_calibration_words; ++i) {
        const double value = next();
        r.calibration[i] = calibration_integer(i) ? uint32_t(int32_t(value)) : std::bit_cast<uint32_t>(float(value));
    }
    return true;
}

KinectRecordFile* kinect_record_nui() {
    static std::mutex lock;
    static std::unique_ptr<KinectRecordFile> file;
    std::lock_guard guard(lock);
    const std::string& prefix = kinect_record_prefix();
    if (prefix.empty()) return nullptr;
    if (!file)
        file = std::make_unique<KinectRecordFile>(prefix + "-nui.csv", kinect_nui_record_header(), kinect_record_limit_bytes());
    return file.get();
}

void KinectNuiRecorder::detector(uint32_t player, uint32_t address, int result, uint32_t primary, uint32_t secondary,
                                 bool has_value, float value) {
    const int index = kinect_nui_detector_index(address);
    if (player >= players || index < 0) return;
    std::lock_guard guard(lock_);
    // Asked more than once in an update (a player's race asks through two
    // objects): the last answer and value, every bit any call added.
    KinectNuiDetectorAnswer& answer = pending_[player].detectors[size_t(index)];
    answer.result = result;
    answer.primary |= primary;
    answer.secondary |= secondary;
    if (has_value) {
        answer.has_value = true;
        answer.value = value;
    }
    ++answer.calls;
}

void KinectNuiRecorder::wipe_y(uint32_t player, float value) {
    if (player >= players) return;
    std::lock_guard guard(lock_);
    pending_[player].has_wipe_y = true;
    pending_[player].wipe_y = value;
}

void KinectNuiRecorder::lean(uint32_t player, float depth, float tilt) {
    if (player >= players) return;
    std::lock_guard guard(lock_);
    pending_[player].has_lean = true;
    pending_[player].lean_depth = depth;
    pending_[player].lean_tilt = tilt;
}

void KinectNuiRecorder::calibration(uint32_t player, const char* step,
                                    const std::array<uint32_t, kinect_nui_calibration_words>& words) {
    if (player >= players || !step || !*step) return;
    std::lock_guard guard(lock_);
    pending_[player].calibration_step = step;
    pending_[player].calibration = words;
}

void KinectNuiRecorder::take(uint32_t player, KinectNuiRecord& row) {
    if (player >= players) return;
    std::lock_guard guard(lock_);
    KinectNuiRecord& pending = pending_[player];
    row.detectors = pending.detectors;
    row.has_lean = pending.has_lean;
    row.lean_depth = pending.lean_depth;
    row.lean_tilt = pending.lean_tilt;
    row.has_wipe_y = pending.has_wipe_y;
    row.wipe_y = pending.wipe_y;
    row.calibration_step = pending.calibration_step;
    row.calibration = pending.calibration;
    pending = {};
}

KinectNuiRecorder& kinect_nui_recorder() {
    static KinectNuiRecorder recorder;
    return recorder;
}

}
