#include "kinect_record_raw.h"

#include "kinect_record.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>

namespace sfr {

namespace {
constexpr char file_magic[8] = {'S', 'F', 'R', 'K', 'R', 'A', 'W', '1'};
constexpr char frame_magic[4] = {'K', 'R', 'W', '1'};
constexpr uint32_t fixed_header_size = 64;
constexpr uint32_t body_size = 24;

void put8(std::string& out, uint8_t value) { out += char(value); }
void put16(std::string& out, uint16_t value) {
    out += char(uint8_t(value));
    out += char(uint8_t(value >> 8));
}
void put32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out += char(uint8_t(value >> (8 * i)));
}
void put64(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out += char(uint8_t(value >> (8 * i)));
}
void put_float(std::string& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    put32(out, bits);
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }
float get_float(const uint8_t* p) {
    const uint32_t bits = get32(p);
    float value = 0;
    std::memcpy(&value, &bits, 4);
    return value;
}
uint64_t header_bytes(uint32_t width, uint32_t height) { return fixed_header_size + 8ull * width * height; }
}

const char* kinect_raw_rays_name(KinectRawRays rays) {
    switch (rays) {
    case KinectRawRays::sensor_table: return "sensor-table";
    case KinectRawRays::sensor_lens: return "sensor-lens";
    case KinectRawRays::typical_lens: return "typical-lens";
    default: return "unknown";
    }
}

std::string kinect_raw_file_header(const KinectRawHeader& header) {
    std::string out(file_magic, 8);
    const size_t pixels = size_t(header.width) * header.height;
    out.reserve(size_t(header_bytes(header.width, header.height)));
    put32(out, kinect_raw_file_version);
    put32(out, uint32_t(header_bytes(header.width, header.height)));
    put32(out, header.width);
    put32(out, header.height);
    put32(out, uint32_t(header.rays_from));
    for (float value : header.lens) put_float(out, value);
    put32(out, kinect_raw_frame_header_size);
    put32(out, 0);
    for (size_t i = 0; i < 2 * pixels; ++i) put_float(out, i < header.rays.size() ? header.rays[i] : 0.0f);
    return out;
}

void kinect_raw_runs_encode(const uint8_t* pixels, size_t count, std::vector<std::pair<uint8_t, uint16_t>>& runs) {
    runs.clear();
    for (size_t i = 0; i < count;) {
        const uint8_t value = pixels[i];
        uint32_t run = 0;
        while (i < count && pixels[i] == value && run < 0xFFFF) ++run, ++i;
        runs.emplace_back(value, uint16_t(run));
    }
}

bool kinect_raw_runs_decode(const std::vector<std::pair<uint8_t, uint16_t>>& runs, size_t count, std::vector<uint8_t>& pixels) {
    pixels.clear();
    pixels.reserve(count);
    for (const auto& [value, run] : runs) {
        if (!run || pixels.size() + run > count) return false;
        pixels.insert(pixels.end(), run, value);
    }
    return pixels.size() == count;
}

std::string kinect_raw_encode_frame(const KinectRawFrame& frame) {
    std::vector<std::pair<uint8_t, uint16_t>> runs;
    kinect_raw_runs_encode(frame.body_index.data(), frame.body_index.size(), runs);
    const uint32_t payload = uint32_t(frame.depth.size() * 2 + 4 + runs.size() * 4);
    std::string out(frame_magic, 4);
    out.reserve(kinect_raw_frame_header_size + payload);
    put32(out, payload);
    put64(out, uint64_t(frame.steady_us));
    put64(out, uint64_t(frame.depth_time));
    put64(out, uint64_t(frame.body_time));
    put64(out, frame.number);
    put32(out, frame.flags);
    const uint32_t bodies = uint32_t(std::min<size_t>(frame.bodies.size(), kinect_raw_max_bodies));
    put32(out, bodies);
    for (float value : frame.floor) put_float(out, value);
    for (uint32_t i = 0; i < kinect_raw_max_bodies; ++i) {
        const KinectRawBody body = i < bodies ? frame.bodies[i] : KinectRawBody{0, 0, 0, 0, {}};
        put64(out, body.tracking_id);
        put8(out, body.body_index);
        put8(out, body.tracked);
        put8(out, body.spine_state);
        put8(out, 0);
        for (float value : body.spine_base) put_float(out, value);
    }
    // The depth in one go (it is most of the frame), little-endian.
    const size_t at = out.size();
    out.resize(at + frame.depth.size() * 2);
    char* p = out.data() + at;
    for (uint16_t value : frame.depth) {
        *p++ = char(uint8_t(value));
        *p++ = char(uint8_t(value >> 8));
    }
    put32(out, uint32_t(runs.size()));
    for (const auto& [value, run] : runs) {
        put8(out, value);
        put8(out, 0);
        put16(out, run);
    }
    return out;
}

bool kinect_raw_parse_header(const uint8_t* bytes, size_t size, KinectRawHeader& header, uint32_t& frame_header_size,
                             std::string& why) {
    if (size < fixed_header_size) { why = "too short for a header"; return false; }
    if (std::memcmp(bytes, file_magic, 8) != 0) { why = "not a Kinect v2 raw recording (SFRKRAW1)"; return false; }
    const uint32_t version = get32(bytes + 8), total = get32(bytes + 12);
    header.width = get32(bytes + 16);
    header.height = get32(bytes + 20);
    header.rays_from = KinectRawRays(get32(bytes + 24));
    for (int i = 0; i < 7; ++i) header.lens[i] = get_float(bytes + 28 + 4 * i);
    frame_header_size = get32(bytes + 56);
    if (version != kinect_raw_file_version) { why = "unknown version"; return false; }
    if (!header.width || !header.height || header.width > 4096 || header.height > 4096) { why = "bad image size"; return false; }
    if (total != header_bytes(header.width, header.height)) { why = "bad header size"; return false; }
    if (frame_header_size < kinect_raw_frame_header_size || frame_header_size > 65536) { why = "bad frame header size"; return false; }
    if (size < total) { why = "header cut short"; return false; }
    const size_t pixels = size_t(header.width) * header.height;
    header.rays.resize(2 * pixels);
    for (size_t i = 0; i < 2 * pixels; ++i) header.rays[i] = get_float(bytes + fixed_header_size + 4 * i);
    return true;
}

KinectRawReader::~KinectRawReader() { close(); }

void KinectRawReader::close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
}

bool KinectRawReader::open(const std::string& path) {
    if (file_) std::fclose(file_);
    file_ = std::fopen(path.c_str(), "rb");
    error_.clear();
    truncated_ = false;
    if (!file_) { error_ = "cannot open " + path; return false; }
    std::vector<uint8_t> bytes(fixed_header_size);
    size_t got = std::fread(bytes.data(), 1, bytes.size(), file_);
    if (got == fixed_header_size && std::memcmp(bytes.data(), file_magic, 8) == 0) {
        const uint32_t total = get32(bytes.data() + 12);
        if (total > fixed_header_size && total < (64u << 20)) {
            bytes.resize(total);
            got += std::fread(bytes.data() + fixed_header_size, 1, total - fixed_header_size, file_);
        }
    }
    if (!kinect_raw_parse_header(bytes.data(), got, header_, frame_header_, error_)) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool KinectRawReader::next(KinectRawFrame& frame) {
    if (!file_) return false;
    std::vector<uint8_t> head(frame_header_);
    const size_t got = std::fread(head.data(), 1, head.size(), file_);
    if (got == 0) return false;
    if (got < head.size()) { truncated_ = true; return false; }
    if (std::memcmp(head.data(), frame_magic, 4) != 0) { error_ = "a broken frame header"; return false; }
    const size_t pixels = size_t(header_.width) * header_.height;
    const uint32_t payload = get32(&head[4]);
    if (payload < pixels * 2 + 4 || payload > pixels * 2 + 4 + pixels * 4) { error_ = "a broken frame size"; return false; }
    frame.steady_us = int64_t(get64(&head[8]));
    frame.depth_time = int64_t(get64(&head[16]));
    frame.body_time = int64_t(get64(&head[24]));
    frame.number = get64(&head[32]);
    frame.flags = get32(&head[40]);
    const uint32_t bodies = get32(&head[44]);
    if (bodies > kinect_raw_max_bodies) { error_ = "too many bodies"; return false; }
    for (int i = 0; i < 4; ++i) frame.floor[i] = get_float(&head[48 + 4 * i]);
    frame.bodies.resize(bodies);
    for (uint32_t i = 0; i < bodies; ++i) {
        const uint8_t* p = &head[64 + body_size * i];
        KinectRawBody& body = frame.bodies[i];
        body.tracking_id = get64(p);
        body.body_index = p[8];
        body.tracked = p[9];
        body.spine_state = p[10];
        for (int k = 0; k < 3; ++k) body.spine_base[k] = get_float(p + 12 + 4 * k);
    }
    std::vector<uint8_t> data(payload);
    if (std::fread(data.data(), 1, payload, file_) < payload) { truncated_ = true; return false; }
    frame.depth.resize(pixels);
    for (size_t i = 0; i < pixels; ++i) frame.depth[i] = get16(&data[2 * i]);
    const uint32_t count = get32(&data[2 * pixels]);
    if (payload != pixels * 2 + 4 + size_t(count) * 4) { error_ = "a broken run count"; return false; }
    std::vector<std::pair<uint8_t, uint16_t>> runs(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = &data[2 * pixels + 4 + 4 * size_t(i)];
        runs[i] = {p[0], get16(p + 2)};
    }
    if (!kinect_raw_runs_decode(runs, pixels, frame.body_index)) { error_ = "broken body index runs"; return false; }
    return true;
}

KinectRawRecordFile::KinectRawRecordFile(std::string path, uint64_t limit_bytes, uint32_t every, KinectRecordQueue* queue)
    : queue_(queue ? *queue : KinectRecordQueue::process()), path_(std::move(path)), limit_(limit_bytes),
      every_(every ? every : 1) {}

KinectRawRecordFile::~KinectRawRecordFile() {
    queue_.drain(this, std::chrono::seconds(10));
    if (file_) std::fclose(file_);
}

bool KinectRawRecordFile::is_open() const {
    std::lock_guard guard(lock_);
    return started_;
}

uint64_t KinectRawRecordFile::frames_written() const {
    std::lock_guard guard(lock_);
    return frames_;
}

bool KinectRawRecordFile::open(const KinectRawHeader& header) {
    std::lock_guard guard(lock_);
    if (started_) return !failed_;
    started_ = true;
    width_ = header.width;
    height_ = header.height;
    rays_from_ = header.rays_from;
    // The header goes first; the file opens on the writer thread.
    std::string bytes = kinect_raw_file_header(header);
    accounted_ = bytes.size();
    if (!queue_.push(this, std::move(bytes))) {
        failed_ = true;
        std::cerr << "NATIVE_KINECT_RECORD raw header dropped (disk too slow): " << path_ << " not written" << std::endl;
        return false;
    }
    return true;
}

bool KinectRawRecordFile::write(const KinectRawFrame& frame) {
    std::lock_guard guard(lock_);
    if (!started_ || failed_ || full_) return false;
    if (offered_++ % every_) return false;
    const size_t pixels = size_t(width_) * height_;
    if (frame.depth.size() != pixels || frame.body_index.size() != pixels) return false;
    std::string bytes = kinect_raw_encode_frame(frame);
    if (accounted_ + bytes.size() > limit_) {
        full_ = true;
        std::cerr << "NATIVE_KINECT_RECORD raw full " << path_ << " after " << accounted_ << " bytes (" << frames_
                  << " frames): stopped" << std::endl;
        return false;
    }
    const uint64_t size = bytes.size();
    if (!queue_.push(this, std::move(bytes))) return false;
    accounted_ += size;
    ++frames_;
    return true;
}

void KinectRawRecordFile::record_write(std::string& bytes) {
    if (!opened_) {
        opened_ = true;
        file_ = std::fopen(path_.c_str(), "wb");
        if (!file_) {
            failed_ = true;
            std::cerr << "NATIVE_KINECT_RECORD failed to open " << path_ << std::endl;
            return;
        }
        uint64_t free_bytes = UINT64_MAX;
        std::error_code error;
        const std::filesystem::path where = std::filesystem::absolute(std::filesystem::path(path_), error).parent_path();
        if (!error) {
            const std::filesystem::space_info space = std::filesystem::space(where, error);
            if (!error) free_bytes = space.available;
        }
        std::cerr << "NATIVE_KINECT_RECORD writing " << path_ << " (v2 raw depth " << width_ << 'x' << height_ << ", rays "
                  << kinect_raw_rays_name(rays_from_) << ", at most " << limit_ / (1024 * 1024) << " MB, "
                  << (every_ == 1 ? std::string("every frame") : "1 frame in " + std::to_string(every_));
        if (free_bytes != UINT64_MAX) std::cerr << ", " << free_bytes / (1024 * 1024) << " MB free";
        std::cerr << ')' << std::endl;
        if (free_bytes != UINT64_MAX && free_bytes < 2 * limit_)
            std::cerr << "NATIVE_KINECT_RECORD raw low disk: " << free_bytes / (1024 * 1024) << " MB free for " << path_
                      << ", less than twice the cap (" << limit_ / (1024 * 1024) << " MB)" << std::endl;
    }
    if (file_) std::fwrite(bytes.data(), 1, bytes.size(), file_);
}

void KinectRawRecordFile::record_flush() {
    if (file_) std::fflush(file_);
}

namespace {
std::mutex& raw_lock() {
    static std::mutex lock;
    return lock;
}
bool& raw_switch() {
    static bool on = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_RAW");
        return text && *text == '1';
    }();
    return on;
}
}

bool kinect_record_raw_enabled() {
    std::lock_guard guard(raw_lock());
    return raw_switch();
}

void kinect_record_raw_enable() {
    std::lock_guard guard(raw_lock());
    raw_switch() = true;
}

uint64_t kinect_record_raw_limit_bytes() {
    static const uint64_t limit = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_RAW_MB");
        const double mb = text && *text ? std::strtod(text, nullptr) : 16384.0;
        return uint64_t((mb >= 1 && mb <= 4194304 ? mb : 16384.0) * 1024 * 1024);
    }();
    return limit;
}

uint32_t kinect_record_raw_every() {
    static const uint32_t every = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_RAW_EVERY");
        const long value = text && *text ? std::strtol(text, nullptr, 10) : 1;
        return uint32_t(value >= 1 && value <= 1000 ? value : 1);
    }();
    return every;
}

std::string kinect_record_raw_path(const std::string& prefix) { return prefix + "-v2raw.bin"; }

KinectRawRecordFile* kinect_record_raw_file() {
    if (!kinect_record_raw_enabled()) return nullptr;
    const std::string& prefix = kinect_record_prefix();
    if (prefix.empty()) return nullptr;
    std::lock_guard guard(raw_lock());
    static std::unique_ptr<KinectRawRecordFile> file;
    if (!file)
        file = std::make_unique<KinectRawRecordFile>(kinect_record_raw_path(prefix), kinect_record_raw_limit_bytes(),
                                                     kinect_record_raw_every());
    return file.get();
}

}
