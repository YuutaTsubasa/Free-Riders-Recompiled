#pragma once
#include "kinect_record.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sfr {

// Recording what a Kinect v2 measured, before it is made into the game's
// v1-format depth (kinect_v2_depth.h), so the depth for the game can be made
// again offline through any view with any settings (sfr_kinect_depth_resynth,
// kinect_v2_resynth.h; docs/kinect-sensor.md). Each frame is exactly what the
// live synthesis used for one image: the depth and body index frames it
// paired (same RelativeTime), the newest body frame's bodies and floor as the
// view keeps them (KinectDepthViewer::bodies), and the times both views step
// on.
//
// SFR_KINECT_RECORD_RAW=1 together with SFR_KINECT_RECORD=<prefix> (the
// probe's --raw sets the switch) writes <prefix>-v2raw.bin, whenever a
// Kinect v2 makes depth for the game (the game, or sfr_kinect_probe).
// SFR_KINECT_RECORD_RAW_MB caps the file (default 16384; "NATIVE_KINECT_RECORD
// raw full" once in the log when reached), SFR_KINECT_RECORD_RAW_EVERY=N keeps
// every Nth frame only (default 1; the ideal and player views follow the
// frames they are given, so only every frame gives the live images back
// exactly).
//
// The file, all little-endian ("SFRKRAW1"):
//   header (header size bytes):
//     0  char[8]  "SFRKRAW1"
//     8  uint32   version (1)
//     12 uint32   header size (64 + 8 * width * height)
//     16 uint32   depth width (512)
//     20 uint32   depth height (424)
//     24 uint32   where the rays come from (KinectRawRays): 1 the sensor's
//                 own table (ICoordinateMapper::GetDepthFrameToCameraSpaceTable),
//                 2 the sensor's lens (GetDepthCameraIntrinsics, without
//                 distortion), 3 a typical v2's lens
//     28 float[7] the sensor's depth lens: focal x, y, centre x, y (pixels),
//                 radial 2, 4, 6 (0 when unknown)
//     56 uint32   frame header size (208)
//     60 uint32   0
//     64 float[2 * width * height] a ray per depth pixel, rows top-down:
//                 x / z, y / z of camera space (metres; +y up)
//   then per frame, a 208-byte frame header:
//     0  char[4]  "KRW1"
//     4  uint32   payload bytes
//     8  int64    steady_us: the host's steady clock when the image was made
//                 (as the depth recording's, kinect_record_depth.h)
//     16 int64    the depth frame's RelativeTime (100 ns; the body index
//                 frame's is within half a frame of it)
//     24 int64    the newest body frame's RelativeTime (100 ns; 0 none yet)
//     32 uint64   number: the made image's number (as in the depth recording)
//     40 uint32   flags: bit 0 the live synthesis had the sensor's mapper's
//                 points (else a typical lens's, kinect_v2_depth_points_from_lens)
//     44 uint32   bodies (0..6)
//     48 float[4] the body frame's floor plane (zero: none)
//     64 6 x 24 bytes, the first `bodies` used: uint64 tracking id (the 32
//                 bits the game gets, kinect_v2_short_tracking_id), uint8
//                 body index (0..5: the body index image's value), uint8
//                 tracked (1), uint8 the SpineBase joint's state (0 not, 1
//                 inferred, 2 tracked), uint8 0, float[3] SpineBase (metres)
//   and the payload: the depth, width x height uint16 millimetres (0 none),
//   then the body index as runs: uint32 run count, then per run uint8 value
//   (0..5, 255 none), uint8 0, uint16 count (1..65535).
// A frame cut short at the end is left out by the reader.
//
// None of this is Windows', so it is tested everywhere (kinect_v2_depth_test).

constexpr uint32_t kinect_raw_file_version = 1;
constexpr uint32_t kinect_raw_frame_header_size = 208;
constexpr uint32_t kinect_raw_max_bodies = 6;
constexpr uint32_t kinect_raw_flag_mapped = 1;

enum class KinectRawRays : uint32_t { unknown = 0, sensor_table = 1, sensor_lens = 2, typical_lens = 3 };
const char* kinect_raw_rays_name(KinectRawRays rays);  // "sensor-table", "sensor-lens", "typical-lens"

struct KinectRawHeader {
    uint32_t width = 512, height = 424;
    KinectRawRays rays_from = KinectRawRays::unknown;
    std::array<float, 7> lens{};  // focal x, y, centre x, y, radial 2, 4, 6
    std::vector<float> rays;      // 2 per pixel
};

struct KinectRawBody {
    uint64_t tracking_id = 0;
    uint8_t body_index = 0;
    uint8_t tracked = 1;
    uint8_t spine_state = 0;
    std::array<float, 3> spine_base{};
};

struct KinectRawFrame {
    int64_t steady_us = 0;
    int64_t depth_time = 0;  // 100 ns
    int64_t body_time = 0;   // 100 ns
    uint64_t number = 0;
    uint32_t flags = 0;
    std::array<float, 4> floor{};
    std::vector<KinectRawBody> bodies;
    std::vector<uint16_t> depth;      // width x height millimetres
    std::vector<uint8_t> body_index;  // width x height
};

std::string kinect_raw_file_header(const KinectRawHeader& header);
std::string kinect_raw_encode_frame(const KinectRawFrame& frame);

// Body index runs: (value, count) pairs, and back (false when they do not
// make exactly count pixels).
void kinect_raw_runs_encode(const uint8_t* pixels, size_t count, std::vector<std::pair<uint8_t, uint16_t>>& runs);
bool kinect_raw_runs_decode(const std::vector<std::pair<uint8_t, uint16_t>>& runs, size_t count, std::vector<uint8_t>& pixels);

class KinectRawReader {
public:
    ~KinectRawReader();
    bool open(const std::string& path);
    void close();
    // False at the end, at a frame cut short, or at a broken one (error()).
    bool next(KinectRawFrame& frame);
    const KinectRawHeader& header() const { return header_; }
    const std::string& error() const { return error_; }
    bool truncated() const { return truncated_; }
private:
    std::FILE* file_ = nullptr;
    KinectRawHeader header_;
    uint32_t frame_header_ = 0;
    std::string error_;
    bool truncated_ = false;
};

// The parse of a whole header held in memory (for the reader and tests).
bool kinect_raw_parse_header(const uint8_t* bytes, size_t size, KinectRawHeader& header, uint32_t& frame_header_size,
                             std::string& why);

// The raw file a recorder writes: started by open() with its header (the
// rays are only known once the sensor has its calibration), bounded in size,
// every Nth frame offered kept. Frames are taken whole from any thread and
// written by the record queue (kinect_record.h: the caller never waits for
// the disk; a frame the queue has no room for is dropped and counted, a gap
// in the frame numbers).
class KinectRawRecordFile : public KinectRecordSink {
public:
    KinectRawRecordFile(std::string path, uint64_t limit_bytes, uint32_t every,
                        KinectRecordQueue* queue = nullptr);  // null: the process's
    ~KinectRawRecordFile() override;  // writes what is queued (at most 10 s), then closes
    bool is_open() const;
    bool open(const KinectRawHeader& header);
    // False when the frame was not taken (not open, skipped by every, full,
    // failed, dropped).
    bool write(const KinectRawFrame& frame);
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
    KinectRawRays rays_from_ = KinectRawRays::unknown;
    bool started_ = false, full_ = false;
    std::atomic<bool> failed_{false};
    // The writer thread's.
    std::FILE* file_ = nullptr;
    bool opened_ = false;
};

// SFR_KINECT_RECORD_RAW=1, or kinect_record_raw_enable() (the probe's --raw).
bool kinect_record_raw_enabled();
void kinect_record_raw_enable();
// SFR_KINECT_RECORD_RAW_MB in bytes (default 16384 MB), and
// SFR_KINECT_RECORD_RAW_EVERY (default 1).
uint64_t kinect_record_raw_limit_bytes();
uint32_t kinect_record_raw_every();
// <prefix>-v2raw.bin.
std::string kinect_record_raw_path(const std::string& prefix);
// The raw file when raw recording is on (with a prefix from
// SFR_KINECT_RECORD or kinect_record_start), null otherwise.
KinectRawRecordFile* kinect_record_raw_file();

}
