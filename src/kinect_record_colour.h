#pragma once
#include "kinect_record.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sfr {

// Recording a Kinect's colour pictures beside its depth images and skeletons
// (docs/kinect-sensor.md, "錄製"), so that what a host without the Kinect
// for Windows runtime has to work from -- colour and depth, no skeleton, no
// player index -- can be studied offline against what the runtime made of
// the same moments (#305, the Kinect on macOS).
//
// SFR_KINECT_RECORD_COLOUR=1 together with SFR_KINECT_RECORD=<prefix> turns
// it on; sfr_kinect_probe's --colour sets the switch. The probe writes each
// sensor's pictures to <prefix>-v1-colour.bin and <prefix>-v2-colour.bin (a
// v2's at the preview's v1 size, kinect_v2_image.h). A picture is 900 KB, so
// SFR_KINECT_RECORD_COLOUR_EVERY=N keeps every Nth (default 2: 15 a second)
// and SFR_KINECT_RECORD_COLOUR_MB caps each file (default 4096;
// "NATIVE_KINECT_RECORD colour full" once in the log when reached).
//
// The file, all little-endian:
//   header, 32 bytes:
//     0  char[8]  "SFRKCOL1"
//     8  uint32   version (1)
//     12 uint32   header size (32)
//     16 uint32   width
//     20 uint32   height
//     24 uint32   pixel format (1: 8-bit blue, green, red)
//     28 uint32   frame header size (40)
//   then per frame, a 40-byte frame header:
//     0  char[4]  "KCF1"
//     4  uint32   payload bytes (width x height x 3)
//     8  int64    steady_us: the host's steady clock (kinect_record_now_us)
//                 when the sensor's backend received the picture, the clock
//                 the depth file and the skeleton CSV share
//     16 int64    handed_us: the same clock when the probe read it
//     24 int64    sdk_time: a v1's liTimeStamp (ms), a v2's RelativeTime
//                 (100 ns); 0 when unknown
//     32 uint64   number: the picture's number (counts from 1 per sensor)
//   and the payload: rows top-down, three bytes a pixel.
// A frame cut short at the end is left out by the reader.
//
// None of this is Windows', so it is tested everywhere (kinect_record_test).

constexpr uint32_t kinect_colour_file_version = 1;
constexpr uint32_t kinect_colour_file_header_size = 32;
constexpr uint32_t kinect_colour_frame_header_size = 40;
constexpr uint32_t kinect_colour_pixel_format_bgr = 1;

struct KinectColourRecordFrame {
    int64_t steady_us = 0, handed_us = 0, sdk_time = 0;
    uint64_t number = 0;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgr;  // width x height x 3
};

// A KinectImage's colour (four bytes a pixel: blue, green, red, unused) into
// a frame's three. False when the picture is not one.
bool kinect_colour_frame_pixels(KinectColourRecordFrame& frame, const uint8_t* bgrx, uint32_t width, uint32_t height,
                                uint32_t bytes_per_pixel);

std::string kinect_colour_file_header(uint32_t width, uint32_t height);
std::string kinect_colour_encode_frame(const KinectColourRecordFrame& frame);

// Reads a colour file frame by frame.
class KinectColourReader {
public:
    ~KinectColourReader();
    bool open(const std::string& path);
    // False at the end, at a frame cut short, or at a broken one (error()).
    bool next(KinectColourRecordFrame& frame);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::string& error() const { return error_; }
    bool truncated() const { return truncated_; }
private:
    std::FILE* file_ = nullptr;
    uint32_t width_ = 0, height_ = 0, frame_header_ = 0;
    std::string error_;
    bool truncated_ = false;
};

// One colour file, as KinectDepthRecordFile is one depth file: opened on the
// first frame, bounded, every Nth frame offered kept, written by the record
// queue.
class KinectColourRecordFile : public KinectRecordSink {
public:
    KinectColourRecordFile(std::string path, uint64_t limit_bytes, uint32_t every,
                           KinectRecordQueue* queue = nullptr);  // null: the process's
    ~KinectColourRecordFile() override;  // writes what is queued (at most 10 s), then closes
    // False when the frame was not taken (skipped by every, full, failed,
    // dropped).
    bool write(const KinectColourRecordFrame& frame);
    const std::string& path() const { return path_; }
    uint64_t frames_written() const;
    void record_write(std::string& bytes) override;
    void record_flush() override;
private:
    KinectRecordQueue& queue_;
    mutable std::mutex lock_;
    std::string path_;
    uint64_t limit_ = 0, accounted_ = 0, offered_ = 0, frames_ = 0;
    uint32_t every_ = 1, width_ = 0, height_ = 0;
    bool started_ = false, full_ = false;
    std::atomic<bool> failed_{false};
    std::FILE* file_ = nullptr;  // the writer thread's
    bool opened_ = false;
};

// SFR_KINECT_RECORD_COLOUR=1, or kinect_record_colour_enable() (the probe's
// --colour) before the first file is asked for.
bool kinect_record_colour_enabled();
void kinect_record_colour_enable();
uint64_t kinect_record_colour_limit_bytes();  // SFR_KINECT_RECORD_COLOUR_MB (default 4096 MB)
uint32_t kinect_record_colour_every();        // SFR_KINECT_RECORD_COLOUR_EVERY (default 2)
// <prefix>-<tag>-colour.bin ("v1", "v2"), <prefix>-colour.bin for no tag.
std::string kinect_record_colour_path(const std::string& prefix, const std::string& tag);
// The colour file for a tag when colour recording is on, null otherwise.
KinectColourRecordFile* kinect_record_colour_file(const std::string& tag);

}
