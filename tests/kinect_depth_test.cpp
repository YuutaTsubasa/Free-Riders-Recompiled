// The depth recording's file format, the title's depth view features
// computed again from an image, and the comparison of two recordings
// (kinect_record_depth.h, kinect_depth_features.h, kinect_depth_compare.h).
#include "kinect_depth_compare.h"
#include "kinect_depth_features.h"
#include "kinect_race_record.h"
#include "kinect_record_depth.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

constexpr uint32_t width = 320, height = 240;
uint16_t pixel(uint32_t millimetres, uint32_t player) { return uint16_t(millimetres << 3 | player); }

std::vector<uint16_t> blank() { return std::vector<uint16_t>(size_t(width) * height, 0); }
void rectangle(std::vector<uint16_t>& image, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, uint16_t value) {
    for (uint32_t y = y0; y <= y1; ++y)
        for (uint32_t x = x0; x <= x1; ++x) image[size_t(y) * width + x] = value;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
uint32_t u32(const std::string& bytes, size_t at) {
    return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at + 1])) << 8 | uint32_t(uint8_t(bytes[at + 2])) << 16 |
           uint32_t(uint8_t(bytes[at + 3])) << 24;
}

sfr::KinectDepthRecordFrame frame_of(const std::vector<uint16_t>& pixels, uint64_t number) {
    sfr::KinectDepthRecordFrame f;
    f.steady_us = 1000000 + int64_t(number) * 33333;
    f.handed_us = f.steady_us + 5000;
    f.sdk_time = int64_t(number) * 333333;
    f.number = number;
    f.sdk_frame = number + 100;
    f.source = sfr::KinectDepthSource::v2;
    f.origin = {0.0f, 0.5f, -1.5f};
    f.z_scale = 1.378f;
    f.width = width;
    f.height = height;
    f.pixels = pixels;
    return f;
}

void header_layout() {
    const std::string header = sfr::kinect_depth_file_header(320, 240);
    require(header.size() == 32 && header.compare(0, 8, "SFRKDEP2") == 0, "magic");
    require(u32(header, 8) == 2 && u32(header, 12) == 32 && u32(header, 16) == 320 && u32(header, 20) == 240 &&
                u32(header, 24) == 1 && u32(header, 28) == 68,
            "version, sizes, pixel format, frame header size");
    uint32_t w = 0, h = 0, frame_header = 0;
    std::string why;
    require(sfr::kinect_depth_parse_file_header(reinterpret_cast<const uint8_t*>(header.data()), header.size(), w, h,
                                                frame_header, why) && w == 320 && h == 240 && frame_header == 68,
            "the header parses");
    std::string old = "SFRKDEP1" + std::string(24, '\0');
    require(!sfr::kinect_depth_parse_file_header(reinterpret_cast<const uint8_t*>(old.data()), old.size(), w, h,
                                                 frame_header, why) && why.find("SFRKDEP1") != std::string::npos,
            "an old SFRKDEP1 file is refused, saying so");
}

void frame_layout() {
    auto image = blank();
    image[0] = 0x1234;
    image[1] = 0xABCD;
    const auto frame = frame_of(image, 7);
    const std::string raw = sfr::kinect_depth_encode_frame(frame, false);
    require(raw.size() == 68 + 320 * 240 * 2, "a raw frame: header and every pixel");
    require(raw.compare(0, 4, "KDF2") == 0 && u32(raw, 4) == 320 * 240 * 2, "frame magic and payload size");
    require(u32(raw, 8) == uint32_t(frame.steady_us) && u32(raw, 16) == uint32_t(frame.handed_us) &&
                u32(raw, 24) == uint32_t(frame.sdk_time) && u32(raw, 32) == 7 && u32(raw, 40) == 107,
            "steady_us, handed_us, sdk_time, number, sdk_frame");
    require(uint8_t(raw[48]) == 2 && uint8_t(raw[49]) == 0, "source v2, raw");
    float z = 0, k = 0;
    std::memcpy(&z, raw.data() + 60, 4);
    std::memcpy(&k, raw.data() + 64, 4);
    require(z == -1.5f && k == 1.378f, "the view");
    require(uint8_t(raw[68]) == 0x34 && uint8_t(raw[69]) == 0x12 && uint8_t(raw[70]) == 0xCD, "pixels little-endian");
    const std::string packed = sfr::kinect_depth_encode_frame(frame);
    require(uint8_t(packed[49]) == 1 && packed.size() < raw.size(), "a mostly empty image goes as zero runs");
}

void zero_runs() {
    std::vector<uint16_t> pixels(200000, 0);
    pixels[3] = 9;
    pixels[199999] = 5;
    std::vector<uint16_t> tokens, back;
    sfr::kinect_depth_zero_runs_encode(pixels.data(), pixels.size(), tokens);
    // 3 zeros, 9, then 199995 zeros (65535 + 65535 + 65535 + 3390), 5.
    require(tokens.size() == 2 + 1 + 8 + 1, "runs longer than 65535 split");
    require(sfr::kinect_depth_zero_runs_decode(tokens.data(), tokens.size(), pixels.size(), back) && back == pixels,
            "and come back");
    require(!sfr::kinect_depth_zero_runs_decode(tokens.data(), tokens.size(), pixels.size() + 1, back),
            "too few pixels is refused");
    const uint16_t broken[] = {0};
    require(!sfr::kinect_depth_zero_runs_decode(broken, 1, 1, back), "a run without its count is refused");
    const std::vector<uint16_t> full(64, 3);
    sfr::kinect_depth_zero_runs_encode(full.data(), full.size(), tokens);
    require(tokens == full, "no zero: the pixels as they are");
}

void files_round_trip() {
    const std::string path = "kinect_depth_test_round_trip.bin";
    std::remove(path.c_str());
    auto image = blank();
    rectangle(image, 100, 139, 50, 149, pixel(2000, 1));
    {
        // Every second image kept; the cap fits three.
        auto first = image;
        first[1] = pixel(1001, 0);
        const uint64_t frame_bytes = sfr::kinect_depth_encode_frame(frame_of(first, 1)).size();
        sfr::KinectDepthRecordFile file(path, 32 + 3 * frame_bytes, 2);
        uint32_t written = 0;
        for (uint64_t n = 1; n <= 9; ++n) {
            auto changed = image;
            changed[n] = pixel(1000 + uint32_t(n), 0);
            written += file.write(frame_of(changed, n));
        }
        require(written == 3 && file.frames_written() == 3, "every second image until the cap");
        require(!file.write(frame_of(image, 10)) && !file.write(frame_of(image, 11)), "a full file takes no more");
    }
    sfr::KinectDepthReader reader;
    require(reader.open(path) && reader.width() == 320 && reader.height() == 240, "the file opens");
    sfr::KinectDepthRecordFrame frame;
    std::vector<uint64_t> numbers;
    while (reader.next(frame)) {
        numbers.push_back(frame.number);
        require(frame.pixels[frame.number] == pixel(1000 + uint32_t(frame.number), 0) &&
                    frame.pixels[60 * width + 120] == pixel(2000, 1),
                "the pixels come back");
        require(frame.source == sfr::KinectDepthSource::v2 && frame.sdk_frame == frame.number + 100 &&
                    frame.handed_us == frame.steady_us + 5000 && frame.z_scale == 1.378f,
                "the clocks and view come back");
    }
    require(numbers == std::vector<uint64_t>{1, 3, 5} && reader.error().empty() && !reader.truncated(),
            "images 1, 3, 5 and a clean end");
    // A game ending mid-write: the last image cut short is left out.
    std::string bytes = read_file(path);
    bytes.resize(bytes.size() - 10);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
    }
    require(reader.open(path), "the cut file opens");
    numbers.clear();
    while (reader.next(frame)) numbers.push_back(frame.number);
    require(numbers == std::vector<uint64_t>{1, 3} && reader.truncated(), "a frame cut short is left out");
    std::remove(path.c_str());
    {
        sfr::KinectDepthRecordFile never("kinect_depth_test_never.bin", 1 << 20, 1);
    }
    std::ifstream none("kinect_depth_test_never.bin");
    require(!none, "no image, no file");
}

void naming_and_space() {
    require(sfr::kinect_record_depth_path("rec\\run1\\v1game", "") == "rec\\run1\\v1game-depth.bin", "the game's file");
    require(sfr::kinect_record_depth_path("rec/v2", "v2") == "rec/v2-v2-depth.bin", "a probe's file per sensor");
    const uint64_t mb = 1024 * 1024;
    require(sfr::kinect_depth_space_warning("x", 9000 * mb, 4096 * mb).empty(), "enough room: no warning");
    const std::string warning = sfr::kinect_depth_space_warning("x", 5000 * mb, 4096 * mb);
    require(warning.find("NATIVE_KINECT_RECORD depth low disk") == 0 && warning.find("5000 MB") != std::string::npos,
            "less than twice the cap: a warning");
}

void empty_image() {
    const auto image = blank();
    const auto f = sfr::kinect_depth_view_features(image.data(), width, height, 0);
    require(f.centroid.column == 160 && f.centroid.row == 120, "no pixel: the centre (82439CE8)");
    require(f.centroid.min_column == 320 && f.centroid.min_row == 240 && f.centroid.max_column == 0 &&
                f.centroid.max_row == 0 && f.centroid.pixels == 0,
            "no pixel: the extents' starting values");
    require(!f.split.written && f.split.counts == std::array<float, 4>{}, "no pixel: counts zero, nothing else");
}

void rectangle_by_hand() {
    // Player 1 (slot 0): columns 100..139, rows 50..149, 2000 mm; player 2
    // and a no-player depth around it, which slot 0 ignores.
    auto image = blank();
    rectangle(image, 0, 319, 0, 239, pixel(3500, 0));
    rectangle(image, 100, 139, 50, 149, pixel(2000, 1));
    rectangle(image, 200, 209, 100, 109, pixel(1500, 2));
    const auto f = sfr::kinect_depth_view_features(image.data(), width, height, 0);
    // 4000 pixels; columns sum 100 * (100 + ... + 139) = 478000, rows 40 *
    // (50 + ... + 149) = 398000: 119.5 and 99.5, truncated.
    require(f.centroid.pixels == 4000 && f.centroid.column == 119 && f.centroid.row == 99, "the centroid, truncated");
    // The largest column as the title has it: lanes e and e + 4 (column % 8)
    // merged by their smaller largest (vminuw, 82439D30): columns 132..139
    // are lanes 4..7, 0..3, so the pairs give 132..135 and the largest is
    // 135, not 139.
    require(f.centroid.min_column == 100 && f.centroid.max_column == 135 && f.centroid.min_row == 50 &&
                f.centroid.max_row == 149,
            "the extents");
    // Upper rows 50..98 (49), lower 99..149 (51); left columns 100..118
    // (19), right 119..139 (21).
    require(f.split.written, "4000 >= 150: written");
    require(f.split.counts[sfr::lower_left] == 51 * 19 && f.split.counts[sfr::lower_right] == 51 * 21 &&
                f.split.counts[sfr::upper_left] == 49 * 19 && f.split.counts[sfr::upper_right] == 49 * 21,
            "the four parts' counts");
    for (float mean : f.split.means) require(near(mean, 16000.0, 0.01), "every part at 2000 mm << 3");
    require(near(f.split.average, 16000.0, 0.01), "the average of the four");
    require(f.split.near_count == 4000, "every pixel within 75 mm of it");
    // The second player in slot 1.
    const auto g = sfr::kinect_depth_view_features(image.data(), width, height, 1);
    require(g.centroid.pixels == 100 && g.centroid.column == 204 && g.centroid.row == 104, "player 2 by its index");
    require(!g.split.written && g.split.counts[0] == 0, "100 pixels < 150: only the zeroed counts");
}

void near_count_by_hand() {
    // Slot 2 (player 3): a 40x40 square at 2000 mm with its right half's
    // lower rows 76 mm (608 units) further: those are not near the average.
    auto image = blank();
    rectangle(image, 60, 99, 40, 79, pixel(2000, 3));
    const auto plain = sfr::kinect_depth_view_features(image.data(), width, height, 2);
    require(plain.centroid.column == 79 && plain.centroid.row == 59 && plain.split.near_count == 1600, "the plain square");
    // The lower right part (rows 59..79, columns 79..99) at 2076 mm.
    rectangle(image, 79, 99, 59, 79, pixel(2076, 3));
    const auto f = sfr::kinect_depth_view_features(image.data(), width, height, 2);
    // The far pixels move the centroid by nothing (only positions count).
    require(f.centroid.column == 79 && f.centroid.row == 59, "positions only");
    // Means: three parts at 16000, the lower right at 16608; average 16152:
    // its pixels are 456 away (near), the others 152 (near).
    require(near(f.split.means[sfr::lower_right], 16608.0, 0.01) && near(f.split.average, 16152.0, 0.01),
            "the average of the means, not of the pixels");
    require(f.split.near_count == 1600, "all within 600 of 16152");
    // 2100 mm (16800): 648 away from (3 * 16000 + 16800) / 4 = 16200: not near.
    rectangle(image, 79, 99, 59, 79, pixel(2100, 3));
    const auto g = sfr::kinect_depth_view_features(image.data(), width, height, 2);
    require(near(g.split.average, 16200.0, 0.01) && g.split.near_count == 1600 - 21 * 21, "the far part is not near");
}

void image_stats() {
    auto image = blank();
    rectangle(image, 0, 159, 0, 239, pixel(2000, 0));
    rectangle(image, 10, 19, 10, 19, pixel(2000, 4));
    const auto s = sfr::kinect_depth_image_stats(image.data(), image.size());
    require(near(s.invalid, 0.5, 1e-6) && s.players[4] == 100 && s.players[0] == image.size() - 100, "holes and players");
}

void series_and_fit() {
    sfr::KinectSeries s;
    s.add(0, 0.0);
    s.add(100, 10.0);
    s.add(300, NAN);
    s.add(400, 40.0);
    require(near(sfr::kinect_series_at(s, 50, 1000), 5.0, 1e-12) && sfr::kinect_series_at(s, 100, 1000) == 10.0,
            "linear between samples, exact on one");
    require(std::isnan(sfr::kinect_series_at(s, 200, 1000)) && std::isnan(sfr::kinect_series_at(s, 500, 1000)) &&
                std::isnan(sfr::kinect_series_at(s, 50, 50)),
            "none next to a missing sample, outside, or across a gap");
    // v1(t) = sin, v2 the same movement 120 ms later, twice as large and 5 up,
    // at another rate and phase: the lag is found, then the line back.
    sfr::KinectSeries a, b;
    const auto movement = [](double seconds) { return std::sin(seconds * 2.1) + 0.4 * std::sin(seconds * 5.3 + 1); };
    for (int i = 0; i < 15 * 20; ++i) a.add(int64_t(i) * 66667, movement(i * 0.066667));
    for (int i = 0; i < 30 * 20; ++i) {
        const int64_t t = 12345 + int64_t(i) * 33333;
        b.add(t, 2.0 * movement(double(t - 120000) / 1e6) + 5.0);
    }
    const sfr::KinectLag lag = sfr::kinect_cross_correlate(a, b, 500000, 10000);
    require(lag.found && near(lag.lag_us, 120000, 6000) && lag.r > 0.99, "the lag: 120 ms, got " + std::to_string(lag.lag_us));
    std::vector<double> truth, other;
    for (size_t i = 0; i < a.t.size(); ++i) {
        truth.push_back(a.v[i]);
        other.push_back(sfr::kinect_series_at(b, a.t[i] + int64_t(std::llround(lag.lag_us)), 100000));
    }
    const sfr::KinectFit fit = sfr::kinect_fit(truth, other);
    require(fit.n > 250 && fit.r > 0.995 && near(fit.slope, 0.5, 0.02) && near(fit.intercept, -2.5, 0.1),
            "v1 = 0.5 v2 - 2.5");
    require(fit.residual_rmse < 0.05 && fit.rmse > 4, "the line explains it; the raw difference does not");
    const sfr::KinectFit exact = sfr::kinect_fit({1, 2, 3, 4}, {3, 5, 7, 9});
    require(exact.n == 4 && near(exact.slope, 0.5, 1e-12) && near(exact.intercept, -0.5, 1e-12) &&
                near(exact.r, 1.0, 1e-12) && near(exact.residual_rmse, 0.0, 1e-12) && near(exact.rmse, std::sqrt(13.5), 1e-12),
            "an exact line");
    sfr::KinectSeries flat;
    for (int i = 0; i < 100; ++i) flat.add(int64_t(i) * 10000, 1.0);
    require(!sfr::kinect_cross_correlate(flat, flat, 100000, 10000).found, "a series that does not move has no lag");
}

void lean() {
    require(sfr::kinect_depth_lean(0, 5) == 1.0 && sfr::kinect_depth_lean(4, 4) == 0.0, "zero +640, and upright");
    require(near(sfr::kinect_depth_lean(15, 10), 0.5, 1e-9) && near(sfr::kinect_depth_lean(10, 15), -0.5, 1e-9),
            "right and left");
    require(sfr::kinect_depth_lean(100, 1) == 3.5 && sfr::kinect_depth_lean(1, 100) == -3.5 &&
                sfr::kinect_depth_lean(5, 0) == 3.5,
            "at most 3.5");
}

void view_check() {
    auto image = blank();
    rectangle(image, 100, 139, 50, 149, pixel(2000, 1));
    const auto tool = sfr::kinect_depth_view_features(image.data(), width, height, 0);
    sfr::KinectDepthViewRecord game;
    game.known = true;
    game.slot = 0;
    game.centroid = tool.centroid;
    game.centroid.pixels = 0;  // the game does not keep it
    game.counts = tool.split.counts;
    game.means = tool.split.means;
    game.near_count = tool.split.near_count;
    game.average = tool.split.average;
    auto check = sfr::kinect_depth_view_check(game, tool);
    require(check.all_equal() && check.compared[15], "the game's own values match");
    game.means[2] = std::nextafter(game.means[2], 1e9f);
    check = sfr::kinect_depth_view_check(game, tool);
    require(!check.all_equal() && !check.equal[12], "one ulp is a difference");
    // A sparse image: the game's means are an older image's, not compared.
    auto sparse = blank();
    rectangle(sparse, 10, 19, 10, 19, pixel(2000, 1));
    const auto little = sfr::kinect_depth_view_features(sparse.data(), width, height, 0);
    sfr::KinectDepthViewRecord stale = game;
    stale.centroid = little.centroid;
    stale.counts = {};
    check = sfr::kinect_depth_view_check(stale, little);
    require(check.all_equal() && !check.compared[10] && !check.compared[15], "stale means are not compared");
    stale.counts[0] = 3;
    require(!sfr::kinect_depth_view_check(stale, little).all_equal(), "but non-zero counts are a difference");
}

void race_columns() {
    const size_t columns = sfr::kinect_record_fields(sfr::kinect_race_record_header()).size();
    require(columns == 14 + 19, "the race header: the old 14 columns and the depth view's 19");
    sfr::KinectRaceRecord row;
    row.steady_us = 5;
    row.record = 0x40002AE0;
    row.view.known = true;
    row.view.frame = 812;
    row.view.steady_us = 4;
    row.view.slot = 3;
    row.view.centroid = {119, 99, 100, 50, 139, 149, 0};
    row.view.counts = {969, 1071, 931, 1029};
    row.view.means = {15999.999f, 16000.0f, 16000.0f, 65528.0f};
    row.view.near_count = 4000;
    row.view.average = 16000.25f;
    std::string text;
    sfr::kinect_race_record_row(row, text);
    sfr::KinectRaceRecord back;
    require(sfr::kinect_parse_race_row(text, back) && back.view.known && back.view.frame == 812 && back.view.slot == 3,
            "the view's columns read back");
    require(back.view.centroid.column == 119 && back.view.centroid.max_row == 149 && back.view.counts == row.view.counts &&
                back.view.means == row.view.means && back.view.near_count == 4000 && back.view.average == 16000.25f,
            "every value exactly (floats through %.9g)");
    sfr::KinectRaceRecord unknown;
    text.clear();
    sfr::kinect_race_record_row(unknown, text);
    require(sfr::kinect_record_fields(text).size() == columns && sfr::kinect_parse_race_row(text, back) && !back.view.known,
            "no depth view yet: empty columns");
    // A row from before the columns.
    const std::string old = "1,2,3,4,1.5,1,1,1,,,2,2,,";
    require(sfr::kinect_parse_race_row(old, back) && !back.view.known && back.original[0] == 1, "an old row still loads");
    sfr::kinect_depth_view_publish(row.record, row.view);
    sfr::KinectDepthViewRecord latest;
    require(sfr::kinect_depth_view_latest(row.record, latest) && latest.frame == 812 &&
                !sfr::kinect_depth_view_latest(row.record + 800, latest),
            "the newest view of a record, none of another");
}

void detector_events() {
    // Two records; crouch runs 0 0 1 1 0 1 for one, 1 1 for the other, with
    // updates where it was not asked (-1) in between.
    std::vector<sfr::KinectRaceRecord> rows;
    const auto add = [&](int64_t t, uint32_t record, int crouch) {
        sfr::KinectRaceRecord& row = rows.emplace_back();
        row.steady_us = t;
        row.record = record;
        row.original[0] = crouch;
    };
    add(10, 1, 0); add(20, 1, 0); add(30, 1, 1); add(35, 2, 1); add(40, 1, -1); add(45, 2, 1);
    add(50, 1, 1); add(60, 1, 0); add(70, 1, 1);
    const auto starts = sfr::kinect_detector_event_starts(rows, 0);
    require(starts == std::vector<int64_t>{30, 35, 70}, "the start of each run of 1, per record, not-asked skipped");
    require(sfr::kinect_detector_event_starts(rows, 2).empty(), "no jumps");
    // A series rising 10 at t = 0 on the v1, 6 on the v2; a second event
    // that the v2 lost.
    std::vector<int64_t> times;
    std::vector<double> truth, other;
    for (int64_t t = -3000000; t <= 8000000; t += 100000) {
        times.push_back(t);
        const bool up = (t >= 200000 && t <= 700000), up2 = (t >= 5200000 && t <= 5600000);
        truth.push_back(100 + (up ? 10 : 0) + (up2 ? -4 : 0));
        other.push_back(t > 4000000 ? std::nan("") : 50 + (up ? 6 : 0));
    }
    const auto a = sfr::kinect_event_amplitudes(times, truth, other, {0, 5000000}, true);
    require(a.events == 2 && a.measured == 1 && a.same_direction == 1, "only events both sensors give count");
    require(std::fabs(a.truth - 10) < 1e-9 && std::fabs(a.other - 6) < 1e-9 && std::fabs(a.ratio() - 0.6) < 1e-9,
            "baseline mean, peak less it, the ratio");
    const auto down = sfr::kinect_event_amplitudes(times, truth, truth, {5000000}, false);
    require(down.measured == 1 && std::fabs(down.truth + 4) < 1e-9, "a jump's response is the smallest value");
}
}

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"header layout", header_layout},
        {"frame layout", frame_layout},
        {"zero runs", zero_runs},
        {"files round trip", files_round_trip},
        {"naming and space", naming_and_space},
        {"empty image", empty_image},
        {"rectangle by hand", rectangle_by_hand},
        {"near count by hand", near_count_by_hand},
        {"image stats", image_stats},
        {"series and fit", series_and_fit},
        {"lean", lean},
        {"view check", view_check},
        {"race columns", race_columns},
        {"detector events", detector_events},
    };
    unsigned failures = 0;
    for (const auto& [name, test] : cases) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    if (failures) return 1;
    std::cout << "kinect_depth: all passed\n";
    return 0;
}
