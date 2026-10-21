#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sfr {

// Recording a real Kinect for offline study (docs/kinect-sensor.md,
// "錄製"): what each sensor tracked, frame by frame, in one CSV schema for a
// Kinect v1 and a Kinect v2, so that the same movement seen by both (two
// sensors opened by one sfr_kinect_probe, or the game and a probe side by
// side) can be lined up by the host's steady clock and compared.
//
// SFR_KINECT_RECORD=<prefix> turns it on, in the game or the probe (the
// probe's --record sets it): <prefix>-skeleton.csv gets one row per tracked
// body per sensor frame (one row with no body when nobody is tracked), as
// the sensor gave it (before any smoothing). The game adds
// <prefix>-race.csv (kinect_race_record.h). Each file stops growing at
// SFR_KINECT_RECORD_MB megabytes (default 512) and says so once in the log.
//
// None of this is Windows', so it is tested everywhere (kinect_record_test).

// The host's steady clock in microseconds: the same clock in every process
// of one machine on Windows (QueryPerformanceCounter), so a game and a
// probe recording side by side share it.
int64_t kinect_record_now_us();

// Kinect v2's JointType order and names, the joints of every row. A v1's
// twenty joints go into the v2 joints defined alike (its hip centre is
// SpineBase, its spine SpineMid, its shoulder centre SpineShoulder); its rows
// leave Neck and the fingertips and thumbs empty.
constexpr uint32_t kinect_record_joint_count = 25;
extern const std::array<const char*, kinect_record_joint_count> kinect_record_joint_names;
// v1's joint (NUI_SKELETON_POSITION_INDEX) in each of those, -1 for none.
extern const std::array<int, kinect_record_joint_count> kinect_record_v1_joint;

struct KinectRecordJoint {
    std::array<float, 3> position{};  // camera space, metres
    int state = -1;                   // 0 not tracked, 1 inferred, 2 tracked; -1 the sensor has no such joint
};

struct KinectRecordBody {
    uint32_t sensor_index = 0;  // the sensor's skeleton slot (0..5), its body index
    uint64_t tracking_id = 0;   // the sensor's own, whole (a v2's is 64 bits)
    std::array<KinectRecordJoint, kinect_record_joint_count> joints{};
    // A v1's skeleton position (its centre); a v2 has none.
    bool has_position = false;
    std::array<float, 3> position{};
    // A v2's hand states (0 unknown, 1 not tracked, 2 open, 3 closed, 4
    // lasso), -1 from a v1.
    std::array<int, 2> hand_states{-1, -1};
    // A v2's clipped edges (kinect_edge bits of kinect_sensor.h: right 1,
    // left 2, top 4, bottom 8; 0 none), -1 from a v1 (it does not say).
    // The last column, so older recordings (without it) still load.
    int clipped_edges = -1;
};

struct KinectRecordFrame {
    int64_t steady_us = 0;   // when the backend received it (kinect_record_now_us)
    std::string sensor;      // "v1" or "v2"
    uint64_t frame = 0;      // the sensor's own frame number (v1's dwFrameNumber), or the backend's count (v2)
    // The SDK's own timestamp: v1's liTimeStamp (milliseconds), v2's
    // RelativeTime (100 ns units); 0 when it gave none.
    int64_t sdk_time = 0;
    std::array<float, 4> floor{};   // floor clip plane, all zero when unknown
    std::array<float, 3> gravity{}; // v1's normal to gravity; a v2 gives none (zero)
    // The share of the newest depth image's pixels without a depth (0..1),
    // that image's number and when it came; negative when no depth is open.
    float depth_invalid = -1;
    uint64_t depth_frame = 0;
    int64_t depth_steady_us = 0;
    std::vector<KinectRecordBody> bodies;
};

// A v1 skeleton's joints and states into a body's v2-ordered joints.
void kinect_record_from_v1(const std::array<std::array<float, 3>, 20>& joints, const std::array<int, 20>& states,
                           KinectRecordBody& body);

// The share of pixels without a depth: a v1's depth-and-player pixels
// carry it above the three player bits (shift 3), a v2's are millimetres
// (shift 0); zero is "no depth" for both, and a v1's "too far" and
// "unknown" values too.
float kinect_depth_invalid_ratio(const uint16_t* pixels, size_t count, int shift);

std::string kinect_record_skeleton_header();
// The frame's rows (each ending in a newline), appended to out.
void kinect_record_skeleton_rows(const KinectRecordFrame& frame, std::string& out);

// One parsed skeleton row (for kinect_record_summary and tests): the frame's
// fields and the body, has_body false for a frame with nobody.
struct KinectRecordRow {
    KinectRecordFrame frame;  // without bodies
    bool has_body = false;
    KinectRecordBody body;
};
bool kinect_parse_skeleton_row(std::string_view line, KinectRecordRow& row);

// A recording's steadiness, for one sensor (sfr_kinect_record_summary,
// docs/kinect-sensor.md): the A/B test of whether a second sensor's light
// makes this one worse, while the player stands still. Each frame's player
// is its nearest body (the smallest SpineBase z).
struct KinectRecordStats {
    uint64_t frames = 0, with_body = 0;
    // Frame-to-frame movement of a tracked joint, in metres RMS divided by
    // the square root of two (the joint's own noise when it is still), for
    // SpineBase, SpineShoulder, HandLeft and HandRight; NaN without pairs.
    std::array<float, 4> jitter{};
    // Of the player's joints the sensor has, the share tracked (not
    // inferred or lost), over frames with a player.
    float tracked = 0;
    // The mean share of depth pixels without a depth; negative when the
    // recording has none.
    float depth_invalid = -1;
    // The player lost: a frame without a body after one with, or another
    // tracking id taking the nearest place.
    uint32_t lost = 0;
    // Frames whose player was cut off by an edge of the picture (a v2's
    // clipped_edges column; a v1's rows never say).
    uint64_t clipped = 0;
};
extern const std::array<uint32_t, 4> kinect_record_jitter_joints;  // SpineBase, SpineShoulder, HandLeft, HandRight
KinectRecordStats kinect_record_stats(const std::vector<KinectRecordRow>& rows, std::string_view sensor);

// Splits one CSV line at its commas (the recorder writes no quoted fields).
std::vector<std::string_view> kinect_record_fields(std::string_view line);

// Where a recorder's bytes go on the writer thread (KinectRecordQueue): the
// file classes below, or a test's fake.
class KinectRecordSink {
public:
    virtual ~KinectRecordSink() = default;
    // Writes one item's bytes (the writer thread only, in the order queued).
    virtual void record_write(std::string& bytes) = 0;
    // After a batch of items: hands what was written to the system.
    virtual void record_flush() = 0;
};

// Recording never waits for the disk: every recorder (the CSV files, the
// depth images, a Kinect v2's raw frames; in the game and in the probe)
// hands an owned buffer to this queue and one writer thread per process
// writes and flushes it (after each batch, so a killed process loses only
// what was still queued). The queue holds at most budget bytes
// (SFR_KINECT_RECORD_QUEUE_MB, default 256): an item that does not fit is
// dropped and counted, never waited for ("NATIVE_KINECT_RECORD dropped=N
// (disk too slow)" at most once a second). A dropped depth or raw image
// leaves a gap in the image numbers and steady_us, which the readers and
// tools take as any gap. An offline tool can ask to wait instead
// (set_blocking).
class KinectRecordQueue {
public:
    explicit KinectRecordQueue(uint64_t budget_bytes);
    ~KinectRecordQueue();  // writes what is queued (at most 10 s), then stops
    // The process's queue (SFR_KINECT_RECORD_QUEUE_MB).
    static KinectRecordQueue& process();
    // Queues bytes for sink; false (dropped and counted) when the budget is
    // full, unless blocking, when it waits for room.
    bool push(KinectRecordSink* sink, std::string&& bytes);
    // Waits (at most wait) until everything queued for sink is written;
    // what is still queued then is dropped (counted). False on time out.
    bool drain(KinectRecordSink* sink, std::chrono::milliseconds wait);
    void set_blocking(bool blocking);
    uint64_t dropped() const;
    uint64_t queued_bytes() const;
    uint64_t budget_bytes() const { return budget_; }
private:
    struct Item {
        KinectRecordSink* sink;
        std::string bytes;
    };
    void run();
    const uint64_t budget_;
    mutable std::mutex lock_;
    std::condition_variable wake_, done_;
    std::deque<Item> items_;
    std::vector<std::pair<KinectRecordSink*, uint64_t>> pending_;  // per sink, queued or being written
    uint64_t queued_ = 0, dropped_ = 0, logged_dropped_ = 0;
    bool stop_ = false, blocking_ = false, started_ = false;
    std::thread writer_;
};
// SFR_KINECT_RECORD_QUEUE_MB, in bytes.
uint64_t kinect_record_queue_budget_bytes();
// " record_dropped=N record_queued_mb=M" for a status line, empty when the
// process never recorded.
std::string kinect_record_queue_status();

// One CSV file the recorder writes, with its header first, bounded in size.
// Rows are taken whole from any thread and written by the record queue; the
// file opens (on the writer thread) with the first row.
class KinectRecordFile : public KinectRecordSink {
public:
    KinectRecordFile(std::string path, std::string header, uint64_t limit_bytes,
                     KinectRecordQueue* queue = nullptr);  // null: the process's
    ~KinectRecordFile() override;  // writes what is queued (at most 10 s), then closes
    // False once the file could not be opened, is full, or the row was
    // dropped (the queue full).
    bool write(const std::string& rows);
    bool write(std::string&& rows);
    const std::string& path() const { return path_; }
    void record_write(std::string& bytes) override;
    void record_flush() override;
private:
    KinectRecordQueue& queue_;
    std::mutex lock_;
    std::string path_, header_;
    uint64_t limit_ = 0, accounted_ = 0;
    bool started_ = false, full_ = false;
    std::atomic<bool> failed_{false};
    // The writer thread's.
    std::FILE* file_ = nullptr;
    bool opened_ = false;
};

// The prefix SFR_KINECT_RECORD names, empty when recording is off.
const std::string& kinect_record_prefix();
// Starts recording to this prefix from now on (the probe's --record); only
// before the first frame is recorded.
void kinect_record_start(const std::string& prefix);
// The skeleton file, or null when recording is off.
KinectRecordFile* kinect_record_skeletons();
// A sensor frame into the skeleton file, when recording.
void kinect_record_skeleton_frame(const KinectRecordFrame& frame);
// SFR_KINECT_RECORD_MB, in bytes.
uint64_t kinect_record_limit_bytes();
// The depth images (SFR_KINECT_RECORD_DEPTH) are kinect_record_depth.h's.

}
