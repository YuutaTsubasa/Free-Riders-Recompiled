#pragma once
#include "kinect_record.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sfr {

// Recording the depth images a Kinect gives the game, for offline study
// (docs/kinect-sensor.md, "錄製"): the same 16-bit depth-and-player-index
// pixels the title's own depth view reads (millimetres << 3 | player index
// 1..6, 0 none), frame by frame, so its features can be computed again
// without the game (kinect_depth_features.h) and a real Kinect v1's images
// compared with the ones a Kinect v2 makes for the game (kinect_v2_depth.h).
//
// SFR_KINECT_RECORD_DEPTH=1 together with SFR_KINECT_RECORD=<prefix> turns
// it on. The game writes every depth image it is handed through
// NuiImageStreamGetNextFrame to <prefix>-depth.bin; sfr_kinect_probe (its
// --depth sets the switch) writes each sensor's own to <prefix>-v1-depth.bin
// and <prefix>-v2-depth.bin (a v1's raw depth, a v2's made one, as the game
// would get them). SFR_KINECT_RECORD_DEPTH_MB caps each file (default 4096;
// "NATIVE_KINECT_RECORD depth full" once in the log when reached) and
// SFR_KINECT_RECORD_DEPTH_EVERY=N keeps every Nth image only (default 1).
//
// The file, all little-endian:
//   header, 32 bytes:
//     0  char[8]  "SFRKDEP2"
//     8  uint32   version (2)
//     12 uint32   header size (32)
//     16 uint32   width (320)
//     20 uint32   height (240)
//     24 uint32   pixel format (1: uint16, millimetres << 3 | player index)
//     28 uint32   frame header size (68)
//   then per frame, a 68-byte frame header:
//     0  char[4]  "KDF2"
//     4  uint32   payload bytes
//     8  int64    steady_us: the host's steady clock (kinect_record_now_us)
//                 when the sensor's backend received the image (a v1) or
//                 made it (a v2); the clock all recordings share
//     16 int64    handed_us: the same clock when the game was handed it
//                 (NuiImageStreamGetNextFrame) or the probe read it
//     24 int64    sdk_time: a v1's liTimeStamp (ms), a v2's RelativeTime
//                 (100 ns) of its depth frame; 0 when unknown
//     32 uint64   number: the image's number, the one the game sees in
//                 NUI_IMAGE_FRAME +8 (counts from 1 per sensor and process)
//     40 uint64   sdk_frame: a v1's dwFrameNumber; 0 for a v2 (none)
//     48 uint8    source: 1 a real Kinect v1, 2 made by a Kinect v2
//     49 uint8    encoding: 0 raw, 1 zero runs
//     50 uint16   0
//     52 float[3] the virtual v1's origin (a v2's KinectDepthView; 0 for a v1)
//     64 float    its depth stretch z_scale (1 for a v1)
//   and the payload: width x height pixels, rows top-down, as uint16 (raw),
//   or as uint16 tokens (zero runs): a non-zero token is a pixel, a zero
//   token is followed by a count (1..65535) of zero pixels.
// A frame cut short at the end (the game ended mid-write) is left out by the
// reader.
//
// None of this is Windows', so it is tested everywhere (kinect_depth_test).

constexpr uint32_t kinect_depth_file_version = 2;
constexpr uint32_t kinect_depth_file_header_size = 32;
constexpr uint32_t kinect_depth_frame_header_size = 68;
constexpr uint32_t kinect_depth_pixel_format_player = 1;

enum class KinectDepthSource : uint8_t { unknown = 0, v1 = 1, v2 = 2 };
enum class KinectDepthEncoding : uint8_t { raw = 0, zero_runs = 1 };

const char* kinect_depth_source_name(KinectDepthSource source);  // "v1", "v2", "unknown"

struct KinectDepthRecordFrame {
    int64_t steady_us = 0;
    int64_t handed_us = 0;
    int64_t sdk_time = 0;
    uint64_t number = 0;
    uint64_t sdk_frame = 0;
    KinectDepthSource source = KinectDepthSource::unknown;
    std::array<float, 3> origin{};
    float z_scale = 1.0f;
    uint32_t width = 0, height = 0;
    std::vector<uint16_t> pixels;  // width x height
};

// The pixels of a KinectImage (2 bytes a pixel, host order: little-endian on
// every host the game runs on) into a frame.
void kinect_depth_frame_pixels(KinectDepthRecordFrame& frame, const uint8_t* bytes, uint32_t width, uint32_t height);

std::string kinect_depth_file_header(uint32_t width, uint32_t height);
// The frame's header and payload; zero runs when they are smaller and allowed.
std::string kinect_depth_encode_frame(const KinectDepthRecordFrame& frame, bool allow_zero_runs = true);

// Zero runs: tokens for pixels, and back (false when the tokens do not make
// exactly count pixels).
void kinect_depth_zero_runs_encode(const uint16_t* pixels, size_t count, std::vector<uint16_t>& tokens);
bool kinect_depth_zero_runs_decode(const uint16_t* tokens, size_t token_count, size_t count, std::vector<uint16_t>& pixels);

// Reads a depth file frame by frame.
class KinectDepthReader {
public:
    ~KinectDepthReader();
    bool open(const std::string& path);
    // False at the end, at a frame cut short, or at a broken one (error()).
    bool next(KinectDepthRecordFrame& frame);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // Why open or next failed; empty at a clean end.
    const std::string& error() const { return error_; }
    // Whether the last frame was cut short (and left out).
    bool truncated() const { return truncated_; }
private:
    std::FILE* file_ = nullptr;
    uint32_t width_ = 0, height_ = 0, frame_header_ = 0;
    std::string error_;
    bool truncated_ = false;
    std::vector<uint16_t> tokens_;
};

// The parse of a header (for the reader and tests).
bool kinect_depth_parse_file_header(const uint8_t* bytes, size_t size, uint32_t& width, uint32_t& height,
                                    uint32_t& frame_header_size, std::string& why);

// The free space of the drive a file goes on: a warning line when it is less
// than twice the cap, else empty.
std::string kinect_depth_space_warning(const std::string& path, uint64_t free_bytes, uint64_t limit_bytes);

// One depth file the recorder writes, opened on the first frame (a sensor
// that never gives one leaves no file), bounded in size, every Nth frame
// offered kept. Frames are taken whole from any thread and written by the
// record queue (kinect_record.h: the caller never waits for the disk; a frame
// the queue has no room for is dropped and counted).
class KinectDepthRecordFile : public KinectRecordSink {
public:
    KinectDepthRecordFile(std::string path, uint64_t limit_bytes, uint32_t every,
                          KinectRecordQueue* queue = nullptr);  // null: the process's
    ~KinectDepthRecordFile() override;  // writes what is queued (at most 10 s), then closes
    // False when the frame was not taken (skipped by every, full, failed,
    // dropped).
    bool write(const KinectDepthRecordFrame& frame);
    const std::string& path() const { return path_; }
    // Frames taken for the file (queued or written).
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
    // The writer thread's.
    std::FILE* file_ = nullptr;
    bool opened_ = false;
};

// SFR_KINECT_RECORD_DEPTH=1, or kinect_record_depth_enable() (the probe's
// --depth) before the first file is asked for.
bool kinect_record_depth_enabled();
void kinect_record_depth_enable();
// SFR_KINECT_RECORD_DEPTH_MB in bytes (default 4096 MB), and
// SFR_KINECT_RECORD_DEPTH_EVERY (default 1).
uint64_t kinect_record_depth_limit_bytes();
uint32_t kinect_record_depth_every();
// <prefix>-depth.bin for an empty tag (the game), <prefix>-<tag>-depth.bin
// otherwise (the probe's "v1" and "v2").
std::string kinect_record_depth_path(const std::string& prefix, const std::string& tag);
// The depth file for a tag when depth recording is on (with a prefix from
// SFR_KINECT_RECORD or kinect_record_start), null otherwise.
KinectDepthRecordFile* kinect_record_depth_file(const std::string& tag);

}
