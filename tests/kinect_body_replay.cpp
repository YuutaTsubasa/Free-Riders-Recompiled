// Plays a Kinect recording made on Windows (sfr_kinect_probe --record
// <prefix> --depth --colour, docs/kinect-sensor.md) through what a Mac makes
// of a Kinect's cameras (kinect_depth_body.h), and measures it against what
// Microsoft's runtime made of the same moments: the skeleton (the recording's
// skeleton CSV) and the player index (the depth file's low three bits).
//
//   sfr_kinect_body_replay <prefix> --sensor v1|v2 [--csv <out.csv>]
//       [--joints model|v1|v2] [--colour-shift x,y] [--joint-agreement m] [--arm-agreement m] [--min-score s] [--step m] [--margin m] [--floor m]
//       [--every N] [--limit N] [--dump N <folder>]
//
// --dump N writes every Nth picture as a BMP to the folder (it is the
// player's video: keep it off the repository): the colour picture with the
// model's points (red) and the runtime's joints through the nominal colour
// lens (green), and beside it the depth with the runtime's player (blue) and
// ours (yellow).
//
// <prefix> is the recording's, so <prefix>-skeleton.csv,
// <prefix>-v1-depth.bin and <prefix>-v1-colour.bin (or v2's) are read.
//
// For each colour picture: the depth image nearest in time loses its player
// index (the runtime's answer) and becomes millimetres; the pose model runs
// on the picture and kinect_depth_body places it on the depth; the player is
// cut from the depth twice, from that skeleton and from the runtime's (so the
// cut's own quality shows apart from the skeleton's). The runtime's skeleton
// nearest in time is the answer for the joints.
//
// A v1's depth is the camera's own (320x240, the nominal lens). A v2's is the
// virtual v1 the probe synthesizes (the fit view: the v2's camera space with
// depth stretched by the frame's z_scale, kinect_v2_depth.h), which is the v2
// seen through a pinhole of focal 285.63 / z_scale once the stretch is taken
// out; its colour is the preview's common view (640x466, kinect_v2_image.h),
// placed on the depth as libfreenect2's registration places a v2's (a
// KinectColourDepthGrid), here through the typical colour lens. What a Mac's
// libfreenect2 itself adds (its 512x424 decode and factory registration) is
// not in the recording.
#include "kinect_depth_body.h"
#include "kinect_record.h"
#include "kinect_record_colour.h"
#include "kinect_record_depth.h"
#include "kinect_v2_image.h"
#include "nui_skeleton.h"
#include "pose_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Point = std::array<float, 3>;

// The recording's v2-ordered joint for each NUI joint: the same number but
// for the shoulder centre, which is SpineShoulder (kinect_record_v1_joint).
uint32_t record_joint(uint32_t nui) { return nui == sfr::nui_joint::shoulder_center ? 20 : nui; }

float distance(const Point& a, const Point& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

struct Stat {
    std::vector<float> values;
    void add(float v) {
        if (std::isfinite(v)) values.push_back(v);
    }
    float quantile(float q) const {
        if (values.empty()) return NAN;
        std::vector<float> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        return sorted[std::min(sorted.size() - 1, size_t(q * float(sorted.size() - 1) + 0.5f))];
    }
    float mean() const {
        if (values.empty()) return NAN;
        double sum = 0;
        for (float v : values) sum += v;
        return float(sum / double(values.size()));
    }
};

// The recording's eight parts (the kit's rec.txt), by seconds from its start.
struct Part {
    const char* name;
    float from, to;
};
constexpr Part parts[] = {{"still", 0, 5},  {"right-hand", 5, 20}, {"lean", 20, 35},  {"crouch", 35, 50},
                          {"jump", 50, 65}, {"step", 65, 75},      {"leave", 75, 90}, {"free", 90, 1e9f}};
constexpr size_t part_count = sizeof parts / sizeof parts[0];

// The colour pixel of a camera-space point, through the runtime's nominal
// colour lens (kinect_depth_body.cpp: the colour camera at the depth's
// place, focal 531.15 at 640x480); a v2's through its typical one.
// The colour pixels a v2's typical colour lens is off by (--colour-shift):
// the replay's own placing of a v2's colour picture, which a Mac does with
// libfreenect2's factory registration instead.
float v2_shift[2] = {0, 0};

bool colour_pixel(const Point& p, bool v2, float& x, float& y) {
    if (!(p[2] > 0.1f)) return false;
    if (v2) {
        const auto at = sfr::kinect_v2_colour_estimate(p, sfr::KinectV2View::common);
        x = at.x + v2_shift[0];
        y = at.y + v2_shift[1];
        return at.visible;
    }
    x = 320.0f + p[0] * 531.15f / p[2];
    y = 240.0f - p[1] * 531.15f / p[2];
    return true;
}

// A 32-bit top-down BMP of width x height BGRA pixels.
void write_bmp(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& bgra) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint8_t header[54] = {'B', 'M'};
    const auto put = [&](int at, uint32_t v) {
        for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i));
    };
    put(2, uint32_t(54 + bgra.size()));
    put(10, 54);
    put(14, 40);
    put(18, width);
    put(22, uint32_t(-int32_t(height)));
    header[26] = 1;
    header[28] = 32;
    put(34, uint32_t(bgra.size()));
    std::fwrite(header, 1, sizeof header, f);
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);
}

void mark(std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, float x, float y, int radius, uint8_t b,
          uint8_t g, uint8_t r) {
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const int px = int(x) + dx, py = int(y) + dy;
            if (px < 0 || py < 0 || px >= int(width) || py >= int(height)) continue;
            uint8_t* at = &bgra[(size_t(py) * width + size_t(px)) * 4];
            at[0] = b;
            at[1] = g;
            at[2] = r;
        }
}

struct Group {
    const char* name;
    std::vector<uint32_t> joints;
};

// The runtime's body for a time: the row nearest it, its nearest body.
const sfr::KinectRecordRow* nearest_row(const std::vector<sfr::KinectRecordRow>& rows, int64_t us) {
    auto at = std::lower_bound(rows.begin(), rows.end(), us,
                               [](const sfr::KinectRecordRow& row, int64_t t) { return row.frame.steady_us < t; });
    const sfr::KinectRecordRow* best = nullptr;
    int64_t best_gap = 0;
    // The rows either side of the time (at is rows.end() past the last one).
    const auto last = at == rows.end() ? at : at + 1;
    for (auto it = at == rows.begin() ? at : at - 1; it != rows.end() && it <= last; ++it) {
        // Rows of one frame share its time; among them the nearest body.
        const int64_t gap = std::llabs(it->frame.steady_us - us);
        if (!best || gap < best_gap ||
            (gap == best_gap && it->has_body &&
             (!best->has_body || it->body.joints[0].position[2] < best->body.joints[0].position[2]))) {
            best = &*it;
            best_gap = gap;
        }
    }
    // Widen to every row of that frame (several bodies are several rows),
    // from its first.
    if (best) {
        auto first = rows.begin() + (best - rows.data());
        while (first != rows.begin() && (first - 1)->frame.frame == best->frame.frame) --first;
        for (auto it = first; it != rows.end() && it->frame.frame == best->frame.frame; ++it)
            if (it->has_body && (!best->has_body || it->body.joints[0].position[2] < best->body.joints[0].position[2]))
                best = &*it;
    }
    return best && best_gap <= 50000 ? best : nullptr;
}

// The overlap of our player with the runtime's: its player (its skeleton
// slot + 1, as the depth's low three bits carry it), or any for 0.
float iou(const std::vector<uint8_t>& ours, const std::vector<uint16_t>& truth_pixels, uint32_t player,
          uint32_t& truth_count) {
    uint32_t both = 0, either = 0;
    truth_count = 0;
    for (size_t i = 0; i < ours.size(); ++i) {
        const uint32_t index = truth_pixels[i] & 7;
        const bool a = ours[i] != 0, b = player ? index == player : index != 0;
        truth_count += b;
        both += a && b;
        either += a || b;
    }
    return either ? float(both) / float(either) : NAN;
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sfr_kinect_body_replay <prefix> --sensor v1|v2 [--csv out.csv] [--joints model|v1|v2] "
                     "[--colour-shift x,y] [--joint-agreement m] [--arm-agreement m] [--min-score s] [--step m] "
                     "[--margin m] [--floor m] [--every N] [--limit N] [--dump N folder]\n";
        return 2;
    }
    const std::string prefix = argv[1];
    std::string sensor = "v1", csv_path;
    sfr::KinectDepthBodySettings body_settings;
    sfr::KinectPlayerCutSettings cut_settings;
    bool joints_given = false;
    int every = 1, limit = 0, dump_every = 0;
    std::string dump_folder;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        const char* value = argv[i + 1];
        if (flag == "--sensor") sensor = value;
        else if (flag == "--csv") csv_path = value;
        else if (flag == "--joint-agreement") body_settings.joint_agreement = std::strtof(value, nullptr);
        else if (flag == "--joints") {
            const std::string places = value;
            body_settings.places = places == "model" ? sfr::KinectJointPlaces::model
                                   : places == "v2"  ? sfr::KinectJointPlaces::v2
                                                     : sfr::KinectJointPlaces::v1;
            joints_given = true;
        } else if (flag == "--colour-shift") std::sscanf(value, "%f,%f", &v2_shift[0], &v2_shift[1]);
        else if (flag == "--arm-agreement") body_settings.arm_agreement = std::strtof(value, nullptr);
        else if (flag == "--min-score") body_settings.min_score = std::strtof(value, nullptr);
        else if (flag == "--step") cut_settings.step_metres = std::strtof(value, nullptr);
        else if (flag == "--margin") cut_settings.margin_metres = std::strtof(value, nullptr);
        else if (flag == "--floor") cut_settings.floor_metres = std::strtof(value, nullptr);
        else if (flag == "--every") every = std::max(1, std::atoi(value));
        else if (flag == "--limit") limit = std::atoi(value);
        else if (flag == "--dump" && i + 2 < argc) {
            dump_every = std::max(1, std::atoi(value));
            dump_folder = argv[++i + 1];
        }
        else {
            std::cerr << "unknown option " << flag << "\n";
            return 2;
        }
    }
    const bool v2 = sensor == "v2";
    // The runtime's joints of the sensor recorded, unless asked otherwise.
    if (!joints_given) body_settings.places = v2 ? sfr::KinectJointPlaces::v2 : sfr::KinectJointPlaces::v1;
    if (!v2 && sensor != "v1") {
        std::cerr << "--sensor is v1 or v2\n";
        return 2;
    }

    std::vector<sfr::KinectRecordRow> rows;
    {
        std::ifstream in(prefix + "-skeleton.csv");
        std::string line;
        while (std::getline(in, line)) {
            sfr::KinectRecordRow row;
            if (sfr::kinect_parse_skeleton_row(line, row) && row.frame.sensor == sensor) rows.push_back(std::move(row));
        }
    }
    sfr::KinectDepthReader depth_reader;
    sfr::KinectColourReader colour_reader;
    if (rows.empty() || !depth_reader.open(prefix + "-" + sensor + "-depth.bin") ||
        !colour_reader.open(prefix + "-" + sensor + "-colour.bin")) {
        std::cerr << "cannot read the recording at " << prefix << " (" << sensor << "): " << depth_reader.error()
                  << colour_reader.error() << "\n";
        return 1;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.frame.steady_us < b.frame.steady_us;
    });
    auto estimator = sfr::PoseEstimator::open(sfr::PoseEstimator::default_model());
    if (!estimator) {
        std::cerr << "no pose model (ONNX Runtime and the MediaPipe model are needed)\n";
        return 1;
    }

    std::FILE* csv = csv_path.empty() ? nullptr : std::fopen(csv_path.c_str(), "w");
    if (csv)
        std::fprintf(csv, "t,colour,depth_gap_ms,truth,found,hip_err,head_err,hand_left_err,hand_right_err,"
                          "hand_left_rel_err,hand_right_rel_err,iou,iou_truth_joints,truth_pixels\n");

    const Group groups[] = {
        {"hip_center", {sfr::nui_joint::hip_center}},
        {"spine", {sfr::nui_joint::spine}},
        {"shoulder_c", {sfr::nui_joint::shoulder_center}},
        {"head", {sfr::nui_joint::head}},
        {"shoulders", {sfr::nui_joint::shoulder_left, sfr::nui_joint::shoulder_right}},
        {"elbows", {sfr::nui_joint::elbow_left, sfr::nui_joint::elbow_right}},
        {"hands", {sfr::nui_joint::hand_left, sfr::nui_joint::hand_right}},
        {"hips", {sfr::nui_joint::hip_left, sfr::nui_joint::hip_right}},
        {"knees", {sfr::nui_joint::knee_left, sfr::nui_joint::knee_right}},
        {"ankles", {sfr::nui_joint::ankle_left, sfr::nui_joint::ankle_right}},
    };
    constexpr size_t group_count = sizeof groups / sizeof groups[0];
    std::array<Stat, group_count> group_error;
    std::array<std::array<Stat, 3>, group_count> group_bias;
    std::array<Stat, 3> hip_bias;  // signed, ours minus the runtime's
    std::array<Stat, 3> hand_rel_bias;
    Stat hand_rel, hand_rel_swapped, mask_iou, mask_iou_truth_joints, depth_gap;
    std::array<Stat, part_count> part_hand_rel, part_iou;
    std::array<uint32_t, part_count> part_truth{}, part_found{};
    uint32_t truth_frames = 0, found_with_truth = 0, empty_frames = 0, found_without_truth = 0, pictures = 0;
    std::array<uint32_t, 3> joint_states{};
    // Colour pixels of the model's well-placed points, and the depth pixels
    // of the runtime's same joints (its skeleton is the depth camera's
    // space, through the nominal depth lens as NuiTransformSkeletonToDepthImage
    // does): what the colour-to-depth mapping should be (REGISTRATION).
    std::vector<std::array<float, 4>> pairs;
    std::array<Stat, 2> colour_shift;  // our tracked joints by state, where the runtime tracked them

    sfr::KinectDepthRecordFrame depth_now, depth_next;
    bool have_now = false, have_next = depth_reader.next(depth_next);
    sfr::KinectColourRecordFrame colour;
    const int64_t start_us = rows.front().frame.steady_us;
    sfr::CameraFrame picture;
    sfr::KinectDepthMillimetres depth;
    std::vector<int> colour_of_depth;
    sfr::KinectColourDepthGrid grid;
    std::vector<uint8_t> index, truth_index;
    uint64_t colour_number = 0;
    while (colour_reader.next(colour)) {
        if (++colour_number % uint64_t(every)) continue;
        if (limit && int(pictures) >= limit) break;
        // The depth nearest the picture.
        while (have_next && depth_next.steady_us <= colour.steady_us) {
            depth_now = std::move(depth_next);
            have_now = true;
            have_next = depth_reader.next(depth_next);
        }
        const sfr::KinectDepthRecordFrame* d = have_now ? &depth_now : nullptr;
        if (have_next && (!d || depth_next.steady_us - colour.steady_us < colour.steady_us - d->steady_us))
            d = &depth_next;
        if (!d) continue;
        ++pictures;
        depth_gap.add(float(std::llabs(d->steady_us - colour.steady_us)) / 1000.0f);

        // The depth as a camera's millimetres, and its lens.
        sfr::KinectDepthCamera camera;
        const float stretch = v2 && d->z_scale > 0 ? d->z_scale : 1.0f;
        camera.focal_x = camera.focal_y = 285.63f / stretch;
        depth.assign(d->pixels.size(), 0);
        for (size_t i = 0; i < depth.size(); ++i) depth[i] = uint16_t(std::lround(float(d->pixels[i] >> 3) / stretch));

        picture.width = colour.width;
        picture.height = colour.height;
        picture.number = colour.number;
        picture.bgra.resize(size_t(colour.width) * colour.height * 4);
        for (size_t i = 0, n = size_t(colour.width) * colour.height; i < n; ++i) {
            std::memcpy(&picture.bgra[4 * i], &colour.bgr[3 * i], 3);
            picture.bgra[4 * i + 3] = 255;
        }

        sfr::PoseLandmarks landmarks{};
        sfr::SkeletonJoints joints{};
        std::array<uint32_t, sfr::nui_joint_count> states{};
        bool found = estimator->estimate(picture, landmarks);
        if (found && v2) {
            colour_of_depth.assign(camera.pixels(), -1);
            for (size_t i = 0; i < camera.pixels(); ++i) {
                if (!depth[i]) continue;
                const Point p = sfr::kinect_depth_pixel_point(camera, float(i % camera.width),
                                                              float(i / camera.width), depth[i] / 1000.0f);
                const auto at = sfr::kinect_v2_colour_estimate(p, sfr::KinectV2View::common);
                const int x = int(at.x + v2_shift[0]), y = int(at.y + v2_shift[1]);
                if (at.visible && x >= 0 && y >= 0 && x < int(colour.width) && y < int(colour.height))
                    colour_of_depth[i] = y * int(colour.width) + x;
            }
            grid.build(colour_of_depth.data(), depth, camera, colour.width, colour.height, 3, false);
            const auto to_depth = [&](float x, float y, float& depth_x, float& depth_y) {
                return grid.find(x, y, depth_x, depth_y);
            };
            found = sfr::kinect_depth_body(landmarks, depth, camera, to_depth, body_settings, joints, states);
        } else if (found) {
            found = sfr::kinect_depth_body(landmarks, depth, body_settings, joints, states);
        }

        const float t = float(colour.steady_us - start_us) / 1e6f;
        size_t part = 0;
        while (part + 1 < part_count && t >= parts[part + 1].from) ++part;
        const sfr::KinectRecordRow* row = nearest_row(rows, colour.steady_us);
        const bool truth = row && row->has_body;
        if (truth) {
            ++truth_frames;
            ++part_truth[part];
            found_with_truth += found;
            part_found[part] += found;
        } else {
            ++empty_frames;
            found_without_truth += found;
        }

        float hip_err = NAN, head_err = NAN, hand_err[2] = {NAN, NAN}, rel_err[2] = {NAN, NAN};
        float ours_iou = NAN, truth_iou = NAN;
        uint32_t truth_pixels = 0;
        const uint32_t truth_player = truth ? row->body.sensor_index + 1 : 0;
        const auto truth_point = [&](uint32_t nui) { return row->body.joints[record_joint(nui)].position; };
        const auto truth_tracked = [&](uint32_t nui) { return row->body.joints[record_joint(nui)].state == 2; };
        if (truth && found) {
            for (size_t g = 0; g < group_count; ++g)
                for (uint32_t j : groups[g].joints)
                    if (truth_tracked(j)) {
                        group_error[g].add(distance(joints[j], truth_point(j)));
                        for (int axis = 0; axis < 3; ++axis) group_bias[g][axis].add(joints[j][axis] - truth_point(j)[axis]);
                        ++joint_states[std::min<uint32_t>(states[j], 2)];
                    }
            if (truth_tracked(sfr::nui_joint::hip_center)) {
                hip_err = distance(joints[0], truth_point(0));
                for (int axis = 0; axis < 3; ++axis) hip_bias[axis].add(joints[0][axis] - truth_point(0)[axis]);
            }
            if (truth_tracked(sfr::nui_joint::head))
                head_err = distance(joints[sfr::nui_joint::head], truth_point(sfr::nui_joint::head));
            // The hands from the shoulder centre: what the menu cursor and
            // the race's reaches read, apart from where the whole body is.
            const uint32_t hands[2] = {sfr::nui_joint::hand_left, sfr::nui_joint::hand_right};
            const uint32_t centre = sfr::nui_joint::shoulder_center;
            for (int h = 0; h < 2; ++h) {
                if (truth_tracked(hands[h])) hand_err[h] = distance(joints[hands[h]], truth_point(hands[h]));
                if (!truth_tracked(hands[h]) || !truth_tracked(centre)) continue;
                const auto rel = [&](const Point& hand, const Point& from) {
                    return Point{hand[0] - from[0], hand[1] - from[1], hand[2] - from[2]};
                };
                const Point truth_rel = rel(truth_point(hands[h]), truth_point(centre));
                const Point ours_rel = rel(joints[hands[h]], joints[centre]);
                rel_err[h] = distance(ours_rel, truth_rel);
                for (int axis = 0; axis < 3; ++axis) hand_rel_bias[axis].add(ours_rel[axis] - truth_rel[axis]);
                hand_rel.add(rel_err[h]);
                part_hand_rel[part].add(rel_err[h]);
                hand_rel_swapped.add(distance(rel(joints[hands[1 - h]], joints[centre]), truth_rel));
            }
        }
        if (truth) {
            using namespace sfr::pose_point;
            // The model's side is the mirrored body's (kinect_depth_body).
            const std::pair<uint32_t, uint32_t> same[] = {
                {elbow_right, sfr::nui_joint::elbow_left}, {elbow_left, sfr::nui_joint::elbow_right},
                {wrist_right, sfr::nui_joint::wrist_left}, {wrist_left, sfr::nui_joint::wrist_right},
                {knee_right, sfr::nui_joint::knee_left},   {knee_left, sfr::nui_joint::knee_right},
                {ankle_right, sfr::nui_joint::ankle_left}, {ankle_left, sfr::nui_joint::ankle_right}};
            for (const auto& [point, joint] : same) {
                const Point p = truth_point(joint);
                if (landmarks[point].score < 0.7f || !truth_tracked(joint) || !(p[2] > 0.5f)) continue;
                if (v2) {
                    // A v2's in the colour picture: where the typical colour
                    // lens puts the runtime's joint, and the model's point.
                    float x = 0, y = 0;
                    if (colour_pixel(p, true, x, y))
                        colour_shift[0].add(landmarks[point].x - x), colour_shift[1].add(landmarks[point].y - y);
                    continue;
                }
                pairs.push_back({landmarks[point].x, landmarks[point].y, 160.0f + p[0] * 285.63f / p[2],
                                 120.0f - p[1] * 285.63f / p[2]});
            }
        }
        // The player's pixels, from our skeleton and from the runtime's.
        if (found) {
            index.assign(depth.size(), 0);
            sfr::kinect_cut_player(depth, camera, joints, 1, cut_settings, index);
            ours_iou = iou(index, d->pixels, truth_player, truth_pixels);
        }
        if (truth) {
            sfr::SkeletonJoints given{};
            for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) given[j] = truth_point(j);
            truth_index.assign(depth.size(), 0);
            sfr::kinect_cut_player(depth, camera, given, 1, cut_settings, truth_index);
            truth_iou = iou(truth_index, d->pixels, truth_player, truth_pixels);
        }
        if (truth_pixels >= 150) {
            if (found) {
                mask_iou.add(ours_iou);
                part_iou[part].add(ours_iou);
            }
            if (truth) mask_iou_truth_joints.add(truth_iou);
        }
        if (dump_every && (pictures - 1) % uint32_t(dump_every) == 0) {
            // The picture, then the depth (scaled to the picture's height) beside it.
            const uint32_t w = colour.width, h = colour.height, dw = camera.width, dh = camera.height;
            const float k = float(h) / float(dh);
            const uint32_t out_w = w + uint32_t(float(dw) * k);
            std::vector<uint8_t> out(size_t(out_w) * h * 4, 0);
            for (uint32_t y = 0; y < h; ++y) std::memcpy(&out[size_t(y) * out_w * 4], &picture.bgra[size_t(y) * w * 4], w * 4);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = w; x < out_w; ++x) {
                    const size_t i = size_t(std::min(dh - 1, uint32_t(float(y) / k))) * dw +
                                     std::min(dw - 1, uint32_t(float(x - w) / k));
                    const uint8_t grey = depth[i] ? uint8_t(255 - std::min(255, int(depth[i]) / 20)) : 0;
                    const bool theirs = (d->pixels[i] & 7) != 0, ours = found && index.size() == depth.size() && index[i];
                    uint8_t* at = &out[(size_t(y) * out_w + x) * 4];
                    at[0] = theirs ? 255 : grey;
                    at[1] = ours ? 255 : grey;
                    at[2] = ours ? 255 : grey;
                    at[3] = 255;
                }
            for (const auto& l : landmarks)
                if (l.score > 0.3f) mark(out, out_w, h, l.x, l.y, 3, 0, 0, 255);
            if (truth)
                for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
                    float x = 0, y = 0;
                    if (colour_pixel(truth_point(j), v2, x, y)) mark(out, out_w, h, x, y, 2, 0, 255, 0);
                }
            if (found)
                for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
                    float x = 0, y = 0;
                    if (colour_pixel(joints[j], v2, x, y)) mark(out, out_w, h, x, y, 1, 255, 0, 255);
                }
            if (truth && found)
                for (uint32_t j = 0; j < sfr::nui_joint_count; ++j) {
                    const Point a = joints[j], b = truth_point(j);
                    float ax = 0, ay = 0, bx = 0, by = 0;
                    colour_pixel(a, v2, ax, ay);
                    colour_pixel(b, v2, bx, by);
                    std::printf("DUMP t=%.2f joint=%2u state=%u/%d ours=%+.3f,%+.3f,%.3f runtime=%+.3f,%+.3f,%.3f "
                                "pixel ours=%.0f,%.0f runtime=%.0f,%.0f\n",
                                t, j, states[j], row->body.joints[record_joint(j)].state, a[0], a[1], a[2], b[0], b[1],
                                b[2], ax, ay, bx, by);
                }
            char name[64];
            std::snprintf(name, sizeof name, "/%06.2f.bmp", t);
            write_bmp(dump_folder + name, out_w, h, out);
        }
        if (csv)
            std::fprintf(csv, "%.3f,%llu,%.1f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u\n", t,
                         (unsigned long long)colour.number,
                         float(std::llabs(d->steady_us - colour.steady_us)) / 1000.0f, truth ? 1 : 0, found ? 1 : 0,
                         hip_err, head_err, hand_err[0], hand_err[1], rel_err[0], rel_err[1], ours_iou, truth_iou,
                         truth_pixels);
    }
    if (csv) std::fclose(csv);

    std::printf("REPLAY sensor=%s pictures=%u depth_gap_ms_median=%.1f\n", sensor.c_str(), pictures,
                depth_gap.quantile(0.5f));
    std::printf("DETECT runtime_body=%u found=%u (%.1f%%) runtime_empty=%u found_anyway=%u\n", truth_frames,
                found_with_truth, truth_frames ? 100.0 * found_with_truth / truth_frames : 0.0, empty_frames,
                found_without_truth);
    std::printf("JOINTS error_m median/p90 where the runtime tracked them:\n");
    for (size_t g = 0; g < group_count; ++g)
        std::printf("  %-10s n=%-5zu median=%.3f p90=%.3f bias x=%+.3f y=%+.3f z=%+.3f\n", groups[g].name,
                    group_error[g].values.size(), group_error[g].quantile(0.5f), group_error[g].quantile(0.9f),
                    group_bias[g][0].quantile(0.5f), group_bias[g][1].quantile(0.5f), group_bias[g][2].quantile(0.5f));
    std::printf("HIP_BIAS_M x=%+.3f y=%+.3f z=%+.3f (ours minus runtime, mean)\n", hip_bias[0].mean(),
                hip_bias[1].mean(), hip_bias[2].mean());
    std::printf("OUR_STATES at runtime-tracked joints: from depth=%u shape=%u\n", joint_states[2], joint_states[1]);
    std::printf("HAND_FROM_SHOULDERS error_m median=%.3f p90=%.3f (sides swapped: median=%.3f) bias x=%+.3f y=%+.3f z=%+.3f\n",
                hand_rel.quantile(0.5f), hand_rel.quantile(0.9f), hand_rel_swapped.quantile(0.5f),
                hand_rel_bias[0].quantile(0.5f), hand_rel_bias[1].quantile(0.5f), hand_rel_bias[2].quantile(0.5f));
    std::printf("PLAYER_MASK iou median=%.3f p10=%.3f; from the runtime's skeleton median=%.3f p10=%.3f\n",
                mask_iou.quantile(0.5f), mask_iou.quantile(0.1f), mask_iou_truth_joints.quantile(0.5f),
                mask_iou_truth_joints.quantile(0.1f));
    if (pairs.size() >= 10) {
        // depth = a colour + b, each axis on its own, by least squares; and
        // the nominal mapping's residual for comparison.
        for (int axis = 0; axis < 2; ++axis) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            const double n = double(pairs.size());
            for (const auto& q : pairs) {
                sx += q[axis];
                sy += q[2 + axis];
                sxx += double(q[axis]) * q[axis];
                sxy += double(q[axis]) * q[2 + axis];
            }
            const double a = (n * sxy - sx * sy) / (n * sxx - sx * sx), b = (sy - a * sx) / n;
            Stat fitted, nominal, offset;
            for (const auto& q : pairs) {
                fitted.add(std::fabs(float(a * q[axis] + b) - q[2 + axis]));
                const auto at = sfr::kinect_colour_to_depth_pixel(q[0], q[1]);
                nominal.add(std::fabs(at[axis] - q[2 + axis]));
                offset.add(q[2 + axis] - at[axis]);
            }
            std::printf("REGISTRATION %c depth=%.5f*colour%+.3f residual_median=%.2f px; nominal residual_median=%.2f "
                        "px, nominal needs %+.2f px (median)\n",
                        axis ? 'y' : 'x', a, b, fitted.quantile(0.5f), nominal.quantile(0.5f), offset.quantile(0.5f));
        }
        std::printf("REGISTRATION pairs=%zu\n", pairs.size());
    }
    if (!colour_shift[0].values.empty())
        std::printf("REGISTRATION v2 colour: the model's points minus the runtime's joints through the lens "
                    "(with --colour-shift %.2f,%.2f): x %+.2f y %+.2f px (median of %zu)\n",
                    v2_shift[0], v2_shift[1], colour_shift[0].quantile(0.5f), colour_shift[1].quantile(0.5f),
                    colour_shift[0].values.size());
    std::printf("PARTS (seconds from the start, the kit's order):\n");
    for (size_t p = 0; p < part_count; ++p)
        std::printf("  %-10s found=%u/%u hand_rel_median=%.3f iou_median=%.3f\n", parts[p].name, part_found[p],
                    part_truth[p], part_hand_rel[p].quantile(0.5f), part_iou[p].quantile(0.5f));
    return 0;
}
