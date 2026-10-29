// The Kinect colour recording (kinect_record_colour.h): pictures written
// through the record queue come back with their clocks, every Nth one kept
// up to the cap, a picture cut short at the end left out.
#include "kinect_record_colour.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr uint32_t width = 64, height = 48;

// A picture whose pixel n is marked, four bytes a pixel as a sensor gives it.
std::vector<uint8_t> bgrx(uint64_t n) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4, 7);
    pixels[4 * n + 0] = uint8_t(n);
    pixels[4 * n + 1] = uint8_t(n + 1);
    pixels[4 * n + 2] = uint8_t(n + 2);
    pixels[4 * n + 3] = 0xEE;  // unused: not kept
    return pixels;
}

sfr::KinectColourRecordFrame frame_of(uint64_t n) {
    sfr::KinectColourRecordFrame frame;
    const auto pixels = bgrx(n);
    require(sfr::kinect_colour_frame_pixels(frame, pixels.data(), width, height, 4), "four bytes a pixel convert");
    frame.steady_us = int64_t(1000000 + n * 33333);
    frame.handed_us = frame.steady_us + 2000;
    frame.sdk_time = int64_t(n * 333333);
    frame.number = n;
    return frame;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void conversion() {
    sfr::KinectColourRecordFrame frame;
    const auto pixels = bgrx(3);
    require(!sfr::kinect_colour_frame_pixels(frame, pixels.data(), width, height, 2), "depth is not a colour picture");
    require(sfr::kinect_colour_frame_pixels(frame, pixels.data(), width, height, 4) &&
                frame.bgr.size() == size_t(width) * height * 3 && frame.bgr[9] == 3 && frame.bgr[10] == 4 &&
                frame.bgr[11] == 5 && frame.bgr[12] == 7,
            "blue, green, red kept, the fourth byte dropped");
}

void round_trip() {
    const std::string path = "kinect_colour_test_round_trip.bin";
    std::remove(path.c_str());
    {
        // Every second picture kept; the cap fits three.
        const uint64_t frame_bytes = sfr::kinect_colour_encode_frame(frame_of(1)).size();
        sfr::KinectColourRecordFile file(path, 32 + 3 * frame_bytes, 2);
        uint32_t written = 0;
        for (uint64_t n = 1; n <= 9; ++n) written += file.write(frame_of(n));
        require(written == 3 && file.frames_written() == 3, "every second picture until the cap");
        require(!file.write(frame_of(10)) && !file.write(frame_of(11)), "a full file takes no more");
        sfr::KinectColourRecordFrame other = frame_of(1);
        other.width = 32;
        require(!file.write(other), "a picture of another size is not taken");
    }
    sfr::KinectColourReader reader;
    require(reader.open(path) && reader.width() == width && reader.height() == height, "the file opens");
    sfr::KinectColourRecordFrame frame;
    std::vector<uint64_t> numbers;
    while (reader.next(frame)) {
        numbers.push_back(frame.number);
        const size_t at = 3 * frame.number;
        require(frame.bgr[at] == uint8_t(frame.number) && frame.bgr[at + 2] == uint8_t(frame.number + 2) &&
                    frame.bgr[0] == 7,
                "the pixels come back");
        require(frame.steady_us == int64_t(1000000 + frame.number * 33333) && frame.handed_us == frame.steady_us + 2000 &&
                    frame.sdk_time == int64_t(frame.number * 333333),
                "the clocks come back");
    }
    require(numbers == std::vector<uint64_t>{1, 3, 5} && reader.error().empty() && !reader.truncated(),
            "pictures 1, 3, 5 and a clean end");
    std::string bytes = read_file(path);
    bytes.resize(bytes.size() - 10);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
    }
    require(reader.open(path), "the cut file opens");
    numbers.clear();
    while (reader.next(frame)) numbers.push_back(frame.number);
    require(numbers == std::vector<uint64_t>{1, 3} && reader.truncated(), "a picture cut short is left out");
    std::remove(path.c_str());
    {
        sfr::KinectColourRecordFile never("kinect_colour_test_never.bin", 1 << 20, 1);
    }
    std::ifstream none("kinect_colour_test_never.bin");
    require(!none, "no picture, no file");
    require(!reader.open("kinect_colour_test_missing.bin") && !reader.error().empty(), "a missing file is said");
}

void naming() {
    require(sfr::kinect_record_colour_path("rec/run1", "v1") == "rec/run1-v1-colour.bin", "a probe's file per sensor");
    require(sfr::kinect_record_colour_path("rec/run1", "") == "rec/run1-colour.bin", "no tag");
}
}

int main() {
    try {
        conversion();
        round_trip();
        naming();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Kinect colour recording checks passed\n";
    return 0;
}
