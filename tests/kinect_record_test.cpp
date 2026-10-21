#include "kinect_nui_record.h"
#include "kinect_race_record.h"
#include "kinect_record.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <thread>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        out.push_back(text.substr(start, end - start));
        start = end == std::string::npos ? text.size() : end + 1;
    }
    return out;
}

void skeleton_rows_round_trip() {
    const size_t columns = sfr::kinect_record_fields(sfr::kinect_record_skeleton_header()).size();
    require(columns == 22 + 4 * 25 + 1, "the header names every column");
    require(sfr::kinect_record_skeleton_header().find(",clipped_edges\n") != std::string::npos, "clipped_edges is the last column");

    // A v2 body: 25 joints, a 64-bit id, hand states, no centre of its own.
    sfr::KinectRecordFrame v2;
    v2.steady_us = 123456789;
    v2.sensor = "v2";
    v2.frame = 41;
    v2.sdk_time = 987654321012LL;
    v2.floor = {0.01f, 0.99f, 0.1f, 0.8f};
    v2.depth_invalid = 0.125f;
    v2.depth_frame = 7;
    v2.depth_steady_us = 123450000;
    sfr::KinectRecordBody body;
    body.sensor_index = 4;
    body.tracking_id = 72057594037931234ULL;
    body.hand_states = {2, 3};
    body.clipped_edges = 6;  // left and top
    for (uint32_t j = 0; j < 25; ++j) body.joints[j] = {{0.01f * j, -0.5f + 0.02f * j, 2.0f + 0.001f * j}, int(j % 3)};
    v2.bodies = {body};
    std::string text;
    sfr::kinect_record_skeleton_rows(v2, text);
    auto rows = lines(text);
    require(rows.size() == 1, "one row per body");
    require(sfr::kinect_record_fields(rows[0]).size() == columns, "a body row has every column");
    sfr::KinectRecordRow parsed;
    require(sfr::kinect_parse_skeleton_row(rows[0], parsed) && parsed.has_body, "a v2 row reads back");
    require(parsed.frame.sensor == "v2" && parsed.frame.frame == 41 && parsed.frame.sdk_time == 987654321012LL &&
                parsed.frame.steady_us == 123456789, "the frame's clocks read back");
    require(near(parsed.frame.depth_invalid, 0.125f) && parsed.frame.depth_frame == 7, "the depth quality reads back");
    require(parsed.body.tracking_id == body.tracking_id && parsed.body.sensor_index == 4, "the whole 64-bit id");
    require(!parsed.body.has_position && parsed.body.hand_states[0] == 2 && parsed.body.hand_states[1] == 3, "hands");
    require(parsed.body.clipped_edges == 6, "the clipped edges read back");
    {
        // A recording from before the column (no trailing field) still loads, as unknown.
        std::string old_row = rows[0];
        old_row.erase(old_row.rfind(','));
        sfr::KinectRecordRow older;
        require(sfr::kinect_parse_skeleton_row(old_row, older) && older.has_body && older.body.clipped_edges == -1 &&
                    older.body.hand_states[1] == 3, "an older row without the column loads");
    }
    for (uint32_t j = 0; j < 25; ++j)
        require(parsed.body.joints[j].state == int(j % 3) && near(parsed.body.joints[j].position[1], -0.5f + 0.02f * j, 1e-5f),
                "every joint and its state");

    // A v1 skeleton: its joints in the v2 joints alike, Neck and the hands' tips empty.
    std::array<std::array<float, 3>, 20> v1_joints{};
    std::array<int, 20> v1_states{};
    for (int j = 0; j < 20; ++j) {
        v1_joints[j] = {float(j), 0, 2};
        v1_states[j] = j == 7 ? 1 : 2;
    }
    sfr::KinectRecordFrame v1;
    v1.steady_us = 5;
    v1.sensor = "v1";
    v1.gravity = {0, 1, 0};
    sfr::KinectRecordBody old;
    old.has_position = true;
    old.position = {0.1f, 0.2f, 2.1f};
    sfr::kinect_record_from_v1(v1_joints, v1_states, old);
    require(old.joints[20].position[0] == 2.0f && old.joints[20].state == 2, "v1's shoulder centre is SpineShoulder");
    require(old.joints[2].state < 0 && old.joints[21].state < 0 && old.joints[24].state < 0, "v1 has no neck or fingertips");
    require(old.joints[0].position[0] == 0.0f && old.joints[19].position[0] == 19.0f, "the rest keep their numbers");
    require(old.joints[7].state == 1, "an inferred v1 joint stays inferred");
    v1.bodies = {old};
    text.clear();
    sfr::kinect_record_skeleton_rows(v1, text);
    require(sfr::kinect_parse_skeleton_row(lines(text)[0], parsed) && parsed.body.has_position &&
                near(parsed.body.position[2], 2.1f) && parsed.body.joints[2].state < 0 && parsed.frame.depth_invalid < 0,
            "a v1 row reads back in the same schema");
    require(parsed.body.clipped_edges == -1 && sfr::kinect_record_fields(lines(text)[0]).back().empty(),
            "a v1 row leaves clipped_edges empty (unknown)");

    // Nobody tracked: still a row, so lost tracking shows.
    sfr::KinectRecordFrame empty;
    empty.sensor = "v2";
    empty.steady_us = 9;
    text.clear();
    sfr::kinect_record_skeleton_rows(empty, text);
    rows = lines(text);
    require(rows.size() == 1 && sfr::kinect_record_fields(rows[0]).size() == columns, "an empty frame is one full row");
    require(sfr::kinect_parse_skeleton_row(rows[0], parsed) && !parsed.has_body, "with no body");
    require(!sfr::kinect_parse_skeleton_row(sfr::kinect_record_skeleton_header(), parsed), "the header is not a row");
}

void depth_holes() {
    // v1: depth above three player bits; 0, "too far" and "unknown" have none.
    const uint16_t v1[6] = {0, uint16_t(2000 << 3 | 1), uint16_t(0x0FFF << 3), uint16_t(0x1FFF << 3), uint16_t(1500 << 3), 3};
    require(near(sfr::kinect_depth_invalid_ratio(v1, 6, 3), 4.0f / 6.0f), "v1 holes");
    const uint16_t v2[4] = {0, 2000, 0, 800};
    require(near(sfr::kinect_depth_invalid_ratio(v2, 4, 0), 0.5f), "v2 holes are zero millimetres");
    require(sfr::kinect_depth_invalid_ratio(nullptr, 0, 0) < 0, "no image, no ratio");
}

void race_rows_round_trip() {
    // The detectors' 14 columns, then the depth view's 19 (kinect_depth_test).
    require(sfr::kinect_record_fields(sfr::kinect_race_record_header()).size() == 14 + 19, "the race header");
    // A session as the game records it: the title's depth lean pair and what
    // its detectors answered, which the race used as they were.
    std::string text;
    for (int i = 0; i < 5; ++i) {
        sfr::KinectRaceRecord row;
        row.steady_us = int64_t(i) * 33333;
        row.generation = uint64_t(i + 1);
        row.record = 0x71730000;
        row.tracking_id = 9;
        row.depth_right = 1.25f;
        row.depth_left = 1.0f;
        row.original[0] = i < 3 ? 2 : 1;  // the title's own crouch detector
        row.result[0] = row.original[0];
        row.original[2] = 2;
        row.result[2] = 2;
        sfr::kinect_race_record_row(row, text);
    }
    const auto rows = lines(text);
    require(rows.size() == 5, "a row per update");
    sfr::KinectRaceRecord row;
    require(sfr::kinect_parse_race_row(rows[4], row), "every race row reads back");
    require(row.generation == 5 && row.record == 0x71730000 && row.tracking_id == 9 && near(row.depth_right, 1.25f) &&
                near(row.depth_left, 1.0f),
            "the title's depth lean pair is kept");
    require(row.original[0] == 1 && row.result[0] == 1 && row.original[2] == 2 && row.original[1] == -1 &&
                row.result[3] == -1,
            "the detectors' answers and the ones not asked");
    require(!sfr::kinect_parse_race_row(sfr::kinect_race_record_header(), row), "the header is not a row");
}

void stats_measure_jitter_and_losses() {
    // 30 frames still, the hips alternating 2 mm: their noise is 1 mm.
    std::vector<sfr::KinectRecordRow> rows;
    for (int f = 0; f < 30; ++f) {
        sfr::KinectRecordRow row;
        row.frame.sensor = "v1";
        row.frame.frame = uint64_t(f + 1);
        row.frame.steady_us = int64_t(f) * 33333;
        row.frame.depth_invalid = 0.2f;
        row.has_body = f != 20;  // lost once
        row.body.tracking_id = 5;
        for (auto& joint : row.body.joints) joint = {{0, 0, 2}, 2};
        row.body.joints[2].state = -1;  // a v1: no neck
        row.body.joints[11].state = f % 2 ? 1 : 2;  // the right hand half inferred
        row.body.joints[0].position[0] = f % 2 ? 0.002f : 0.0f;
        rows.push_back(row);
    }
    // Another sensor's rows do not count.
    sfr::KinectRecordRow other = rows[0];
    other.frame.sensor = "v2";
    other.body.joints[0].position[0] = 5;
    rows.insert(rows.begin() + 3, other);
    const sfr::KinectRecordStats stats = sfr::kinect_record_stats(rows, "v1");
    require(stats.frames == 30 && stats.with_body == 29 && stats.lost == 1, "frames, bodies and one loss");
    require(near(stats.jitter[0], 0.002f / std::sqrt(2.0f), 1e-6f), "frame-to-frame jitter");
    require(near(stats.jitter[1], 0.0f) && std::isnan(stats.jitter[3]), "still joints and a hand never tracked twice");
    require(near(stats.tracked, (24.0f - 0.5f) / 24.0f, 0.02f), "the tracked share of the joints a v1 has");
    require(near(stats.depth_invalid, 0.2f), "mean depth holes");
}

void files_stay_bounded() {
    const std::string path = "kinect_record_test_bounded.csv";
    {
        sfr::KinectRecordFile file(path, "a,b\n", 20);
        require(file.write("1,2\n") && file.write("3,4\n"), "rows within the limit");
        require(!file.write("5,6,7,8,9,10\n") && !file.write("1,1\n"), "a full file takes nothing more");
    }
    std::ifstream in(path);
    const std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(written == "a,b\n1,2\n3,4\n", "header and whole rows only");
    in.close();
    std::remove(path.c_str());
}
}

// A sink for the record queue: keeps what it is given, and can be stalled
// (a disk that does not answer) until released.
struct FakeSink : sfr::KinectRecordSink {
    std::mutex lock;
    std::condition_variable changed;
    bool stalled = false, inside = false;
    std::vector<std::string> written;
    int flushes = 0;
    void record_write(std::string& bytes) override {
        std::unique_lock guard(lock);
        inside = true;
        changed.notify_all();
        changed.wait(guard, [&] { return !stalled; });
        written.push_back(bytes);
        inside = false;
    }
    void record_flush() override {
        std::lock_guard guard(lock);
        ++flushes;
    }
    void stall(bool on) {
        {
            std::lock_guard guard(lock);
            stalled = on;
        }
        changed.notify_all();
    }
    void wait_inside() {
        std::unique_lock guard(lock);
        changed.wait(guard, [&] { return inside; });
    }
    size_t count() {
        std::lock_guard guard(lock);
        return written.size();
    }
};

void queue_keeps_order() {
    sfr::KinectRecordQueue queue(1 << 20);
    FakeSink a, b;
    for (int i = 0; i < 500; ++i) {
        require(queue.push(&a, "a" + std::to_string(i)) && queue.push(&b, "b" + std::to_string(i)), "taken");
    }
    require(queue.drain(&a, std::chrono::seconds(10)) && queue.drain(&b, std::chrono::seconds(10)), "drained");
    require(a.written.size() == 500 && b.written.size() == 500, "every item written");
    for (int i = 0; i < 500; ++i)
        require(a.written[i] == "a" + std::to_string(i) && b.written[i] == "b" + std::to_string(i), "in the order queued");
    require(a.flushes > 0 && queue.dropped() == 0 && queue.queued_bytes() == 0, "flushed, nothing dropped or left");
}

void queue_never_waits_for_a_stalled_disk() {
    sfr::KinectRecordQueue queue(1000);
    FakeSink sink;
    sink.stall(true);
    require(queue.push(&sink, std::string(300, 'x')), "the first item");
    sink.wait_inside();  // the writer is stuck in it
    uint32_t taken = 0, dropped = 0;
    double slowest_us = 0;
    for (int i = 0; i < 200; ++i) {
        const auto before = std::chrono::steady_clock::now();
        const bool ok = queue.push(&sink, std::string(300, 'y'));
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count();
        slowest_us = std::max(slowest_us, us);
        (ok ? taken : dropped) += 1;
    }
    std::cout << "kinect_record: the slowest push with the disk stalled took " << slowest_us << " us\n";
    // 300 bytes being written count until written: two more fit in 1000.
    require(taken == 2 && dropped == 198 && queue.dropped() == 198, "the budget holds; the rest dropped and counted");
    require(slowest_us < 50000, "a push never waits for the disk");
    sink.stall(false);
    require(queue.drain(&sink, std::chrono::seconds(10)) && sink.count() == 3, "what was taken is written");
    // A drain that runs out of time drops what has not started.
    sink.stall(true);
    queue.push(&sink, "1");
    sink.wait_inside();
    queue.push(&sink, "2");
    queue.push(&sink, "3");
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        sink.stall(false);
    });
    const bool drained = queue.drain(&sink, std::chrono::milliseconds(50));
    release.join();
    require(!drained && sink.count() == 4 && queue.dropped() == 200, "out of time: the queued ones dropped");
}

void file_written_on_destruction() {
    const std::string path = "kinect_record_test_queued.csv";
    std::string expected = "h\n";
    {
        sfr::KinectRecordQueue queue(64 << 20);
        sfr::KinectRecordFile file(path, "h\n", 1 << 20, &queue);
        for (int i = 0; i < 1000; ++i) {
            const std::string row = std::to_string(i) + ",x\n";
            expected += row;
            require(file.write(row), "taken");
        }
    }  // the file drains, then closes
    std::ifstream in(path, std::ios::binary);
    const std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(written == expected, "every row, in order, after the header");
    in.close();
    std::remove(path.c_str());
}

void nui_rows_round_trip() {
    const std::string header = sfr::kinect_nui_record_header();
    const size_t columns = sfr::kinect_record_fields(header.substr(0, header.size() - 1)).size();
    require(columns == 6 + 14 + 24 + 2 + 4 * sfr::kinect_nui_detector_count + 2 + 34, "the header names every column");
    require(header.find(",right_cursor_x,") != std::string::npos && header.find(",steer_value,") != std::string::npos &&
                header.find(",calib_132\n") != std::string::npos,
            "cursor, detector and calibration columns");
    for (const auto& detector : sfr::kinect_nui_detectors)
        require(sfr::kinect_nui_detector_index(detector.address) >= 0, "every detector has its columns");
    require(sfr::kinect_nui_detector_index(0x822C9050) == 0 && sfr::kinect_nui_detector_index(0x12345678) < 0, "by address");

    // The recorder: several calls of one detector in an update keep the last
    // answer and every bit; taking a row starts the next one empty.
    sfr::KinectNuiRecorder recorder;
    recorder.detector(1, 0x822C8778, 2, 0, 0x2000, false, 0);
    recorder.detector(1, 0x822C8778, 1, 0x7000, 0, false, 0);
    recorder.detector(1, 0x822C9BF0, 1, 0x00400100, 0, true, 75.0f);
    recorder.detector(0, 0x822C9050, 2, 0, 0, false, 0);
    recorder.lean(1, -1.25f, 0.5f);
    std::array<uint32_t, sfr::kinect_nui_calibration_words> words{};
    for (uint32_t i = 0; i < words.size(); ++i) words[i] = std::bit_cast<uint32_t>(0.25f * float(i));
    words[28] = 161;  // +112, the centroid column
    words[29] = uint32_t(-3);
    recorder.calibration(1, "body", words);
    sfr::KinectNuiRecord row;
    row.steady_us = 42;
    row.update = 1234;
    row.player = 1;
    row.tracking_id = 77;
    row.body_slot = 3;
    row.racing = true;
    row.has_cursor = true;
    row.hands[1] = {8, 101.5f, 0.42f, 600.0f, 300.0f, 590.25f, 310.5f};
    row.has_joints = true;
    for (uint32_t i = 0; i < sfr::kinect_nui_joint_count; ++i) row.joints[i] = {-0.1f * float(i), 0.2f, -2.5f};
    recorder.take(1, row);
    require(row.detectors[1].result == 1 && row.detectors[1].primary == 0x7000 && row.detectors[1].secondary == 0x2000 &&
                row.detectors[1].calls == 2,
            "the last answer, every bit");
    require(row.detectors[0].result < 0, "another player's answer stays theirs");
    std::string text;
    sfr::kinect_nui_record_row(row, text);
    require(text.back() == '\n' && sfr::kinect_record_fields(text.substr(0, text.size() - 1)).size() == columns,
            "a row has every column");
    sfr::KinectNuiRecord parsed;
    require(sfr::kinect_parse_nui_row(text.substr(0, text.size() - 1), parsed), "a row reads back");
    require(parsed.update == 1234 && parsed.player == 1 && parsed.tracking_id == 77 && parsed.body_slot == 3 && parsed.racing,
            "who and when");
    require(parsed.has_cursor && parsed.hands[1].flags == 8 && near(parsed.hands[1].theta, 101.5f) &&
                near(parsed.hands[1].cursor_y, 310.5f) && near(parsed.hands[1].r, 0.42f),
            "the cursor");
    require(parsed.has_joints && near(parsed.joints[7][0], -0.7f) && near(parsed.joints[7][2], -2.5f), "the joints");
    require(parsed.has_lean && near(parsed.lean_depth, -1.25f) && near(parsed.lean_tilt, 0.5f), "the lean pair");
    require(parsed.detectors[1].result == 1 && parsed.detectors[1].primary == 0x7000 && !parsed.detectors[1].has_value,
            "a detector's answer");
    require(parsed.detectors[10].result == 1 && parsed.detectors[10].has_value && near(parsed.detectors[10].value, 75.0f),
            "the brake's strength");
    require(parsed.detectors[0].result == -1 && !parsed.has_wipe_y, "a detector not asked is empty");
    require(parsed.calibration_step == "body" && parsed.calibration[28] == 161 && int32_t(parsed.calibration[29]) == -3 &&
                near(std::bit_cast<float>(parsed.calibration[33]), 8.25f),
            "the calibration, whole numbers at +112..+120");
    sfr::KinectNuiRecord next;
    recorder.take(1, next);
    require(next.detectors[1].result < 0 && !next.has_lean && next.calibration_step.empty(), "the next row starts empty");
    // A row with nothing but its clocks still has every column.
    sfr::KinectNuiRecord bare;
    std::string bare_text;
    sfr::kinect_nui_record_row(bare, bare_text);
    sfr::KinectNuiRecord bare_parsed;
    require(sfr::kinect_parse_nui_row(bare_text.substr(0, bare_text.size() - 1), bare_parsed) && !bare_parsed.has_cursor &&
                !bare_parsed.has_joints && bare_parsed.calibration_step.empty(),
            "an empty row reads back empty");
    require(!sfr::kinect_parse_nui_row(header.substr(0, header.size() - 1), bare_parsed), "the header is not a row");
}

int main() {
    try {
        nui_rows_round_trip();
        queue_keeps_order();
        queue_never_waits_for_a_stalled_disk();
        file_written_on_destruction();
        skeleton_rows_round_trip();
        depth_holes();
        race_rows_round_trip();
        stats_measure_jitter_and_losses();
        files_stay_bounded();
    } catch (const std::exception& error) {
        std::cerr << "kinect_record_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "kinect_record_test passed\n";
    return 0;
}
