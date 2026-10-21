#include "kinect_record.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace sfr {

int64_t kinect_record_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const std::array<const char*, kinect_record_joint_count> kinect_record_joint_names = {
    "spine_base", "spine_mid", "neck", "head",
    "shoulder_left", "elbow_left", "wrist_left", "hand_left",
    "shoulder_right", "elbow_right", "wrist_right", "hand_right",
    "hip_left", "knee_left", "ankle_left", "foot_left",
    "hip_right", "knee_right", "ankle_right", "foot_right",
    "spine_shoulder", "hand_tip_left", "thumb_left", "hand_tip_right", "thumb_right",
};

// v1's 0..19 are v2's 0..19 except its shoulder centre (2), which is v2's
// SpineShoulder (20); v2's Neck (2) and 21..24 have no v1 joint.
const std::array<int, kinect_record_joint_count> kinect_record_v1_joint = {
    0, 1, -1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 2, -1, -1, -1, -1,
};

void kinect_record_from_v1(const std::array<std::array<float, 3>, 20>& joints, const std::array<int, 20>& states,
                           KinectRecordBody& body) {
    for (uint32_t j = 0; j < kinect_record_joint_count; ++j) {
        const int from = kinect_record_v1_joint[j];
        body.joints[j] = from < 0 ? KinectRecordJoint{} : KinectRecordJoint{joints[from], states[from] < 0 ? 0 : states[from]};
    }
}

float kinect_depth_invalid_ratio(const uint16_t* pixels, size_t count, int shift) {
    if (!pixels || !count) return -1;
    size_t invalid = 0;
    // A v1 (SDK 1.8 NuiImageCamera.h) also has distinct "too far" (0x0FFF)
    // and "unknown" (0x1FFF) values when asked for them; neither is a depth.
    for (size_t i = 0; i < count; ++i) {
        const uint32_t depth = pixels[i] >> shift;
        invalid += depth == 0 || (shift == 3 && (depth == 0x0FFF || depth == 0x1FFF));
    }
    return float(double(invalid) / double(count));
}

namespace {
void put(std::string& out, const char* text) {
    out += ',';
    out += text;
}
void put_float(std::string& out, float value) {
    out += ',';
    if (!std::isfinite(value)) return;  // empty: not a number the reader can use
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", double(value));
    out += text;
}
void put_int(std::string& out, long long value) {
    out += ',';
    out += std::to_string(value);
}
void put_uint(std::string& out, unsigned long long value) {
    out += ',';
    out += std::to_string(value);
}
}

std::string kinect_record_skeleton_header() {
    std::string out = "steady_us,sensor,frame,sdk_time,floor_a,floor_b,floor_c,floor_d,gravity_x,gravity_y,gravity_z,"
                      "depth_invalid,depth_frame,depth_steady_us,bodies,body_index,tracking_id,"
                      "position_x,position_y,position_z,hand_left_state,hand_right_state";
    for (const char* name : kinect_record_joint_names)
        for (const char* part : {"_x", "_y", "_z", "_state"}) {
            out += ',';
            out += name;
            out += part;
        }
    out += ",clipped_edges\n";
    return out;
}

void kinect_record_skeleton_rows(const KinectRecordFrame& frame, std::string& out) {
    const auto row = [&](const KinectRecordBody* body) {
        out += std::to_string(frame.steady_us);
        put(out, frame.sensor.c_str());
        put_uint(out, frame.frame);
        put_int(out, frame.sdk_time);
        for (float v : frame.floor) put_float(out, v);
        for (float v : frame.gravity) put_float(out, v);
        if (frame.depth_invalid >= 0) {
            put_float(out, frame.depth_invalid);
            put_uint(out, frame.depth_frame);
            put_int(out, frame.depth_steady_us);
        } else {
            out += ",,,";
        }
        put_uint(out, frame.bodies.size());
        if (!body) {
            // Nobody: the body's columns empty.
            out.append(8 + 4 * kinect_record_joint_count, ',');
            out += '\n';
            return;
        }
        put_uint(out, body->sensor_index);
        put_uint(out, body->tracking_id);
        if (body->has_position)
            for (float v : body->position) put_float(out, v);
        else
            out += ",,,";
        for (int state : body->hand_states)
            if (state >= 0) put_int(out, state);
            else out += ',';
        for (const KinectRecordJoint& joint : body->joints) {
            if (joint.state < 0) { out += ",,,,"; continue; }
            for (float v : joint.position) put_float(out, v);
            put_int(out, joint.state);
        }
        if (body->clipped_edges >= 0) put_int(out, body->clipped_edges);
        else out += ',';
        out += '\n';
    };
    if (frame.bodies.empty()) row(nullptr);
    for (const KinectRecordBody& body : frame.bodies) row(&body);
}

std::vector<std::string_view> kinect_record_fields(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    std::vector<std::string_view> fields;
    size_t start = 0;
    for (;;) {
        const size_t comma = line.find(',', start);
        fields.push_back(line.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return fields;
}

namespace {
bool number(std::string_view text, double& value) {
    if (text.empty()) return false;
    const std::string copy(text);
    char* end = nullptr;
    value = std::strtod(copy.c_str(), &end);
    return end && *end == 0;
}
// Whole numbers past a double's 53 bits (a v2's tracking ids, its
// RelativeTime) read exactly.
bool whole(std::string_view text, long long& value) {
    if (text.empty()) return false;
    const std::string copy(text);
    char* end = nullptr;
    value = std::strtoll(copy.c_str(), &end, 10);
    return end && *end == 0;
}
bool whole(std::string_view text, unsigned long long& value) {
    if (text.empty() || text[0] == '-') return false;
    const std::string copy(text);
    char* end = nullptr;
    value = std::strtoull(copy.c_str(), &end, 10);
    return end && *end == 0;
}
}

bool kinect_parse_skeleton_row(std::string_view line, KinectRecordRow& row) {
    const auto fields = kinect_record_fields(line);
    // 22 + the joints, and clipped_edges after them; recordings made before
    // that column still load (clipped_edges stays -1 for them).
    constexpr size_t columns = 22 + 4 * kinect_record_joint_count;
    if (fields.size() != columns && fields.size() != columns + 1) return false;
    double v = 0;
    row = {};
    KinectRecordFrame& f = row.frame;
    long long signed_value = 0;
    unsigned long long unsigned_value = 0;
    if (!whole(fields[0], signed_value)) return false;  // the header, or a broken line
    f.steady_us = signed_value;
    f.sensor = std::string(fields[1]);
    if (whole(fields[2], unsigned_value)) f.frame = unsigned_value;
    if (whole(fields[3], signed_value)) f.sdk_time = signed_value;
    for (int i = 0; i < 4; ++i) f.floor[i] = number(fields[4 + i], v) ? float(v) : 0.0f;
    for (int i = 0; i < 3; ++i) f.gravity[i] = number(fields[8 + i], v) ? float(v) : 0.0f;
    f.depth_invalid = number(fields[11], v) ? float(v) : -1.0f;
    if (whole(fields[12], unsigned_value)) f.depth_frame = unsigned_value;
    if (whole(fields[13], signed_value)) f.depth_steady_us = signed_value;
    const size_t bodies = number(fields[14], v) ? size_t(v) : 0;
    row.has_body = number(fields[15], v);
    if (!row.has_body) return true;
    (void)bodies;
    KinectRecordBody& b = row.body;
    b.sensor_index = uint32_t(v);
    if (whole(fields[16], unsigned_value)) b.tracking_id = unsigned_value;
    b.has_position = number(fields[17], v);
    for (int i = 0; i < 3; ++i) b.position[i] = number(fields[17 + i], v) ? float(v) : 0.0f;
    for (int i = 0; i < 2; ++i) b.hand_states[i] = number(fields[20 + i], v) ? int(v) : -1;
    for (uint32_t j = 0; j < kinect_record_joint_count; ++j) {
        const size_t at = 22 + 4 * j;
        KinectRecordJoint& joint = b.joints[j];
        if (!number(fields[at + 3], v)) { joint = {}; continue; }
        joint.state = int(v);
        for (int i = 0; i < 3; ++i) joint.position[i] = number(fields[at + i], v) ? float(v) : NAN;
    }
    if (fields.size() == columns + 1 && number(fields[columns], v)) b.clipped_edges = int(v);
    return true;
}

const std::array<uint32_t, 4> kinect_record_jitter_joints = {0, 20, 7, 11};

KinectRecordStats kinect_record_stats(const std::vector<KinectRecordRow>& rows, std::string_view sensor) {
    KinectRecordStats stats;
    // Rows of one frame are together; the player is its nearest body.
    struct Player {
        bool present = false;
        KinectRecordBody body;
        int64_t steady_us = 0;
    };
    std::vector<Player> players;
    double depth_sum = 0;
    uint64_t depth_count = 0;
    for (size_t i = 0; i < rows.size();) {
        if (rows[i].frame.sensor != sensor) { ++i; continue; }
        const uint64_t number = rows[i].frame.frame;
        Player player;
        player.steady_us = rows[i].frame.steady_us;
        if (rows[i].frame.depth_invalid >= 0) {
            depth_sum += rows[i].frame.depth_invalid;
            ++depth_count;
        }
        size_t j = i;
        for (; j < rows.size() && rows[j].frame.sensor == sensor && rows[j].frame.frame == number &&
               rows[j].frame.steady_us == rows[i].frame.steady_us;
             ++j) {
            if (!rows[j].has_body) continue;
            const float z = rows[j].body.joints[0].position[2];
            if (!player.present || z < player.body.joints[0].position[2]) {
                player.present = true;
                player.body = rows[j].body;
            }
        }
        players.push_back(player);
        i = j;
    }
    stats.frames = players.size();
    std::array<double, 4> squares{};
    std::array<uint64_t, 4> pairs{};
    double tracked = 0;
    for (size_t f = 0; f < players.size(); ++f) {
        const Player& now = players[f];
        if (f > 0) {
            const Player& before = players[f - 1];
            if (before.present && !now.present) ++stats.lost;
            if (before.present && now.present && before.body.tracking_id != now.body.tracking_id) ++stats.lost;
        }
        if (!now.present) continue;
        ++stats.with_body;
        if (now.body.clipped_edges > 0) ++stats.clipped;
        uint32_t have = 0, seen = 0;
        for (const KinectRecordJoint& joint : now.body.joints) {
            if (joint.state < 0) continue;
            ++have;
            seen += joint.state == 2;
        }
        if (have) tracked += double(seen) / have;
        if (f == 0) continue;
        const Player& before = players[f - 1];
        // Consecutive frames of the same body, at most 0.1 s apart.
        if (!before.present || before.body.tracking_id != now.body.tracking_id ||
            now.steady_us - before.steady_us > 100000)
            continue;
        for (size_t k = 0; k < kinect_record_jitter_joints.size(); ++k) {
            const auto& a = before.body.joints[kinect_record_jitter_joints[k]];
            const auto& b = now.body.joints[kinect_record_jitter_joints[k]];
            if (a.state != 2 || b.state != 2) continue;
            double d = 0;
            for (int c = 0; c < 3; ++c) d += double(b.position[c] - a.position[c]) * (b.position[c] - a.position[c]);
            squares[k] += d;
            ++pairs[k];
        }
    }
    for (size_t k = 0; k < 4; ++k)
        stats.jitter[k] = pairs[k] ? float(std::sqrt(squares[k] / double(pairs[k]) / 2.0)) : NAN;
    stats.tracked = stats.with_body ? float(tracked / double(stats.with_body)) : 0.0f;
    stats.depth_invalid = depth_count ? float(depth_sum / double(depth_count)) : -1.0f;
    return stats;
}

namespace {
std::atomic<KinectRecordQueue*> process_queue{nullptr};
}

uint64_t kinect_record_queue_budget_bytes() {
    static const uint64_t budget = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_QUEUE_MB");
        const double mb = text && *text ? std::strtod(text, nullptr) : 256.0;
        return uint64_t((mb >= 1 && mb <= 65536 ? mb : 256.0) * 1024 * 1024);
    }();
    return budget;
}

KinectRecordQueue& KinectRecordQueue::process() {
    // Never destroyed: files kept in other statics drain into it while the
    // process ends, whatever the order their destructors run in.
    static KinectRecordQueue* queue = [] {
        auto* made = new KinectRecordQueue(kinect_record_queue_budget_bytes());
        process_queue.store(made);
        return made;
    }();
    return *queue;
}

std::string kinect_record_queue_status() {
    const KinectRecordQueue* queue = process_queue.load();
    if (!queue) return {};
    char text[96];
    std::snprintf(text, sizeof text, " record_dropped=%llu record_queued_mb=%.1f", (unsigned long long)queue->dropped(),
                  double(queue->queued_bytes()) / (1024.0 * 1024.0));
    return text;
}

KinectRecordQueue::KinectRecordQueue(uint64_t budget_bytes) : budget_(budget_bytes) {}

KinectRecordQueue::~KinectRecordQueue() {
    {
        std::unique_lock guard(lock_);
        // What is queued gets ten seconds to reach the disk.
        done_.wait_for(guard, std::chrono::seconds(10), [&] { return items_.empty() && pending_.empty(); });
        stop_ = true;
        dropped_ += items_.size();
        items_.clear();
    }
    wake_.notify_all();
    if (writer_.joinable()) writer_.join();
}

void KinectRecordQueue::set_blocking(bool blocking) {
    std::lock_guard guard(lock_);
    blocking_ = blocking;
}

uint64_t KinectRecordQueue::dropped() const {
    std::lock_guard guard(lock_);
    return dropped_;
}

uint64_t KinectRecordQueue::queued_bytes() const {
    std::lock_guard guard(lock_);
    return queued_;
}

bool KinectRecordQueue::push(KinectRecordSink* sink, std::string&& bytes) {
    std::unique_lock guard(lock_);
    if (stop_) return false;
    if (!started_) {
        started_ = true;
        writer_ = std::thread([this] { run(); });
    }
    const auto fits = [&] { return queued_ == 0 || queued_ + bytes.size() <= budget_; };
    if (!fits()) {
        if (!blocking_) {
            ++dropped_;
            return false;
        }
        done_.wait(guard, [&] { return fits() || stop_; });
        if (stop_) return false;
    }
    queued_ += bytes.size();
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const auto& p) { return p.first == sink; });
    if (it == pending_.end()) pending_.emplace_back(sink, 1);
    else ++it->second;
    items_.push_back({sink, std::move(bytes)});
    guard.unlock();
    wake_.notify_one();
    return true;
}

bool KinectRecordQueue::drain(KinectRecordSink* sink, std::chrono::milliseconds wait) {
    std::unique_lock guard(lock_);
    const auto none = [&] {
        return std::none_of(pending_.begin(), pending_.end(), [&](const auto& p) { return p.first == sink; });
    };
    if (done_.wait_for(guard, wait, none)) return true;
    // Out of time: what has not started is dropped; what is being written
    // is waited for (its sink is about to go away).
    uint64_t removed = 0;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->sink != sink) { ++it; continue; }
        queued_ -= it->bytes.size();
        it = items_.erase(it);
        ++removed;
    }
    dropped_ += removed;
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
        if (it->first == sink) {
            it->second -= removed;
            if (!it->second) pending_.erase(it);
            break;
        }
    done_.wait(guard, none);
    return false;
}

void KinectRecordQueue::run() {
    auto logged_at = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    std::deque<Item> batch;
    std::vector<KinectRecordSink*> touched;
    for (;;) {
        std::unique_lock guard(lock_);
        wake_.wait_for(guard, std::chrono::seconds(1), [&] { return !items_.empty() || stop_; });
        const auto now = std::chrono::steady_clock::now();
        if (dropped_ != logged_dropped_ && now - logged_at >= std::chrono::seconds(1)) {
            // At most once a second, never from the recorders' own threads.
            std::cerr << "NATIVE_KINECT_RECORD dropped=" << dropped_ << " (disk too slow)" << std::endl;
            logged_dropped_ = dropped_;
            logged_at = now;
        }
        if (items_.empty()) {
            if (stop_) return;
            continue;
        }
        batch.swap(items_);
        guard.unlock();
        touched.clear();
        for (Item& item : batch) {
            item.sink->record_write(item.bytes);
            if (std::find(touched.begin(), touched.end(), item.sink) == touched.end()) touched.push_back(item.sink);
        }
        // Handed to the system after each batch: a game ending without its
        // destructors, or killed, loses only what was still queued.
        for (KinectRecordSink* sink : touched) sink->record_flush();
        guard.lock();
        for (const Item& item : batch) {
            queued_ -= item.bytes.size();
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
                if (it->first == item.sink) {
                    if (!--it->second) pending_.erase(it);
                    break;
                }
        }
        batch.clear();
        guard.unlock();
        done_.notify_all();
    }
}

KinectRecordFile::KinectRecordFile(std::string path, std::string header, uint64_t limit_bytes, KinectRecordQueue* queue)
    : queue_(queue ? *queue : KinectRecordQueue::process()), path_(std::move(path)), header_(std::move(header)),
      limit_(limit_bytes) {}

KinectRecordFile::~KinectRecordFile() {
    queue_.drain(this, std::chrono::seconds(10));
    if (file_) std::fclose(file_);
}

bool KinectRecordFile::write(const std::string& rows) { return write(std::string(rows)); }

bool KinectRecordFile::write(std::string&& rows) {
    std::lock_guard guard(lock_);
    if (failed_ || full_) return false;
    const uint64_t header = started_ ? 0 : header_.size();
    if (accounted_ + header + rows.size() > limit_) {
        full_ = true;
        std::cerr << "NATIVE_KINECT_RECORD full " << path_ << " after " << accounted_ << " bytes: stopped" << std::endl;
        return false;
    }
    const uint64_t size = rows.size();
    // The file opens with the first row (on the writer thread), so turning
    // recording on without a sensor leaves no empty file behind; the header
    // goes with that row.
    std::string bytes = header ? header_ + rows : std::move(rows);
    if (!queue_.push(this, std::move(bytes))) return false;
    started_ = true;
    accounted_ += header + size;
    return true;
}

void KinectRecordFile::record_write(std::string& bytes) {
    if (!opened_) {
        opened_ = true;
        file_ = std::fopen(path_.c_str(), "wb");
        if (!file_) {
            failed_ = true;
            std::cerr << "NATIVE_KINECT_RECORD failed to open " << path_ << std::endl;
            return;
        }
        std::cerr << "NATIVE_KINECT_RECORD writing " << path_ << " (at most " << limit_ / (1024 * 1024) << " MB)"
                  << std::endl;
    }
    if (file_) std::fwrite(bytes.data(), 1, bytes.size(), file_);
}

void KinectRecordFile::record_flush() {
    if (file_) std::fflush(file_);
}

namespace {
std::mutex& prefix_lock() {
    static std::mutex lock;
    return lock;
}
std::string& prefix_value() {
    static std::string prefix = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD");
        return std::string(text && *text && std::string_view(text) != "0" ? text : "");
    }();
    return prefix;
}
std::unique_ptr<KinectRecordFile>& skeleton_file() {
    static std::unique_ptr<KinectRecordFile> file;
    return file;
}
}

const std::string& kinect_record_prefix() {
    std::lock_guard guard(prefix_lock());
    return prefix_value();
}

void kinect_record_start(const std::string& prefix) {
    std::lock_guard guard(prefix_lock());
    if (skeleton_file()) return;
    prefix_value() = prefix;
}

uint64_t kinect_record_limit_bytes() {
    static const uint64_t limit = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_MB");
        const double mb = text && *text ? std::strtod(text, nullptr) : 512.0;
        return uint64_t((mb >= 1 && mb <= 16384 ? mb : 512.0) * 1024 * 1024);
    }();
    return limit;
}

KinectRecordFile* kinect_record_skeletons() {
    std::lock_guard guard(prefix_lock());
    if (prefix_value().empty()) return nullptr;
    auto& file = skeleton_file();
    if (!file)
        file = std::make_unique<KinectRecordFile>(prefix_value() + "-skeleton.csv", kinect_record_skeleton_header(),
                                                  kinect_record_limit_bytes());
    return file.get();
}

void kinect_record_skeleton_frame(const KinectRecordFrame& frame) {
    KinectRecordFile* file = kinect_record_skeletons();
    if (!file) return;
    std::string rows;
    kinect_record_skeleton_rows(frame, rows);
    file->write(std::move(rows));
}

}
