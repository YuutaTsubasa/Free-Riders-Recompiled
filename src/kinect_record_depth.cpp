#include "kinect_record_depth.h"

#include "kinect_record.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>

namespace sfr {

namespace {
constexpr char file_magic[8] = {'S', 'F', 'R', 'K', 'D', 'E', 'P', '2'};
constexpr char frame_magic[4] = {'K', 'D', 'F', '2'};

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
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }
float get_float(const uint8_t* p) {
    const uint32_t bits = get32(p);
    float value = 0;
    std::memcpy(&value, &bits, 4);
    return value;
}
}

const char* kinect_depth_source_name(KinectDepthSource source) {
    switch (source) {
    case KinectDepthSource::v1: return "v1";
    case KinectDepthSource::v2: return "v2";
    default: return "unknown";
    }
}

void kinect_depth_frame_pixels(KinectDepthRecordFrame& frame, const uint8_t* bytes, uint32_t width, uint32_t height) {
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(size_t(width) * height);
    for (size_t i = 0; i < frame.pixels.size(); ++i) frame.pixels[i] = uint16_t(bytes[2 * i] | bytes[2 * i + 1] << 8);
}

std::string kinect_depth_file_header(uint32_t width, uint32_t height) {
    std::string out(file_magic, 8);
    put32(out, kinect_depth_file_version);
    put32(out, kinect_depth_file_header_size);
    put32(out, width);
    put32(out, height);
    put32(out, kinect_depth_pixel_format_player);
    put32(out, kinect_depth_frame_header_size);
    return out;
}

void kinect_depth_zero_runs_encode(const uint16_t* pixels, size_t count, std::vector<uint16_t>& tokens) {
    tokens.clear();
    for (size_t i = 0; i < count;) {
        if (pixels[i]) {
            tokens.push_back(pixels[i++]);
            continue;
        }
        size_t run = 0;
        while (i < count && !pixels[i] && run < 0xFFFF) ++run, ++i;
        tokens.push_back(0);
        tokens.push_back(uint16_t(run));
    }
}

bool kinect_depth_zero_runs_decode(const uint16_t* tokens, size_t token_count, size_t count, std::vector<uint16_t>& pixels) {
    pixels.clear();
    pixels.reserve(count);
    for (size_t i = 0; i < token_count; ++i) {
        if (tokens[i]) {
            if (pixels.size() >= count) return false;
            pixels.push_back(tokens[i]);
            continue;
        }
        if (++i >= token_count || !tokens[i] || pixels.size() + tokens[i] > count) return false;
        pixels.insert(pixels.end(), tokens[i], uint16_t(0));
    }
    return pixels.size() == count;
}

std::string kinect_depth_encode_frame(const KinectDepthRecordFrame& frame, bool allow_zero_runs) {
    std::vector<uint16_t> tokens;
    bool runs = false;
    if (allow_zero_runs) {
        kinect_depth_zero_runs_encode(frame.pixels.data(), frame.pixels.size(), tokens);
        runs = tokens.size() < frame.pixels.size();
    }
    const std::vector<uint16_t>& payload = runs ? tokens : frame.pixels;
    std::string out(frame_magic, 4);
    out.reserve(kinect_depth_frame_header_size + payload.size() * 2);
    put32(out, uint32_t(payload.size() * 2));
    put64(out, uint64_t(frame.steady_us));
    put64(out, uint64_t(frame.handed_us));
    put64(out, uint64_t(frame.sdk_time));
    put64(out, frame.number);
    put64(out, frame.sdk_frame);
    out += char(uint8_t(frame.source));
    out += char(uint8_t(runs ? KinectDepthEncoding::zero_runs : KinectDepthEncoding::raw));
    out += '\0';
    out += '\0';
    for (float value : frame.origin) put_float(out, value);
    put_float(out, frame.z_scale);
    for (uint16_t value : payload) {
        out += char(uint8_t(value));
        out += char(uint8_t(value >> 8));
    }
    return out;
}

bool kinect_depth_parse_file_header(const uint8_t* bytes, size_t size, uint32_t& width, uint32_t& height,
                                    uint32_t& frame_header_size, std::string& why) {
    if (size < kinect_depth_file_header_size) { why = "too short for a header"; return false; }
    if (std::memcmp(bytes, file_magic, 8) != 0) {
        why = std::memcmp(bytes, "SFRKDEP1", 8) == 0 ? "an old SFRKDEP1 file (no frame numbers): not read"
                                                     : "not a Kinect depth recording";
        return false;
    }
    const uint32_t version = get32(bytes + 8), header = get32(bytes + 12);
    width = get32(bytes + 16);
    height = get32(bytes + 20);
    const uint32_t format = get32(bytes + 24);
    frame_header_size = get32(bytes + 28);
    if (version != kinect_depth_file_version || header != kinect_depth_file_header_size) { why = "unknown version"; return false; }
    if (format != kinect_depth_pixel_format_player) { why = "unknown pixel format"; return false; }
    if (!width || !height || width > 4096 || height > 4096) { why = "bad image size"; return false; }
    if (frame_header_size < kinect_depth_frame_header_size || frame_header_size > 4096) { why = "bad frame header size"; return false; }
    return true;
}

KinectDepthReader::~KinectDepthReader() {
    if (file_) std::fclose(file_);
}

bool KinectDepthReader::open(const std::string& path) {
    if (file_) std::fclose(file_);
    file_ = std::fopen(path.c_str(), "rb");
    error_.clear();
    truncated_ = false;
    if (!file_) { error_ = "cannot open " + path; return false; }
    uint8_t header[kinect_depth_file_header_size];
    const size_t got = std::fread(header, 1, sizeof header, file_);
    if (!kinect_depth_parse_file_header(header, got, width_, height_, frame_header_, error_)) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool KinectDepthReader::next(KinectDepthRecordFrame& frame) {
    if (!file_) return false;
    std::vector<uint8_t> header(frame_header_);
    const size_t got = std::fread(header.data(), 1, header.size(), file_);
    if (got == 0) return false;
    if (got < header.size()) { truncated_ = true; return false; }
    if (std::memcmp(header.data(), frame_magic, 4) != 0) { error_ = "a broken frame header"; return false; }
    const uint32_t bytes = get32(&header[4]);
    const size_t count = size_t(width_) * height_;
    if (bytes % 2 || bytes > count * 4 + 16) { error_ = "a broken frame size"; return false; }
    frame.steady_us = int64_t(get64(&header[8]));
    frame.handed_us = int64_t(get64(&header[16]));
    frame.sdk_time = int64_t(get64(&header[24]));
    frame.number = get64(&header[32]);
    frame.sdk_frame = get64(&header[40]);
    frame.source = KinectDepthSource(header[48]);
    const auto encoding = KinectDepthEncoding(header[49]);
    for (int i = 0; i < 3; ++i) frame.origin[i] = get_float(&header[52 + 4 * i]);
    frame.z_scale = get_float(&header[64]);
    frame.width = width_;
    frame.height = height_;
    std::vector<uint8_t> payload(bytes);
    if (std::fread(payload.data(), 1, bytes, file_) < bytes) { truncated_ = true; return false; }
    tokens_.resize(bytes / 2);
    for (size_t i = 0; i < tokens_.size(); ++i) tokens_[i] = uint16_t(payload[2 * i] | payload[2 * i + 1] << 8);
    if (encoding == KinectDepthEncoding::raw) {
        if (tokens_.size() != count) { error_ = "a raw frame of the wrong size"; return false; }
        frame.pixels = tokens_;
    } else if (encoding == KinectDepthEncoding::zero_runs) {
        if (!kinect_depth_zero_runs_decode(tokens_.data(), tokens_.size(), count, frame.pixels)) {
            error_ = "broken zero runs";
            return false;
        }
    } else {
        error_ = "unknown encoding";
        return false;
    }
    return true;
}

std::string kinect_depth_space_warning(const std::string& path, uint64_t free_bytes, uint64_t limit_bytes) {
    if (free_bytes >= 2 * limit_bytes) return {};
    return "NATIVE_KINECT_RECORD depth low disk: " + std::to_string(free_bytes / (1024 * 1024)) + " MB free for " + path +
           ", less than twice the cap (" + std::to_string(limit_bytes / (1024 * 1024)) + " MB)";
}

KinectDepthRecordFile::KinectDepthRecordFile(std::string path, uint64_t limit_bytes, uint32_t every,
                                             KinectRecordQueue* queue)
    : queue_(queue ? *queue : KinectRecordQueue::process()), path_(std::move(path)), limit_(limit_bytes),
      every_(every ? every : 1) {}

KinectDepthRecordFile::~KinectDepthRecordFile() {
    queue_.drain(this, std::chrono::seconds(10));
    if (file_) std::fclose(file_);
}

uint64_t KinectDepthRecordFile::frames_written() const {
    std::lock_guard guard(lock_);
    return frames_;
}

bool KinectDepthRecordFile::write(const KinectDepthRecordFrame& frame) {
    std::lock_guard guard(lock_);
    if (failed_ || full_) return false;
    if (offered_++ % every_) return false;
    if (!started_) {
        width_ = frame.width;
        height_ = frame.height;
    }
    if (frame.width != width_ || frame.height != height_ || frame.pixels.size() != size_t(width_) * height_) return false;
    // The header goes with the first frame; the file opens (on the writer
    // thread) when that arrives.
    std::string bytes = started_ ? std::string() : kinect_depth_file_header(width_, height_);
    bytes += kinect_depth_encode_frame(frame);
    if (accounted_ + bytes.size() > limit_) {
        full_ = true;
        std::cerr << "NATIVE_KINECT_RECORD depth full " << path_ << " after " << accounted_ << " bytes (" << frames_
                  << " images): stopped" << std::endl;
        return false;
    }
    const uint64_t size = bytes.size();
    if (!queue_.push(this, std::move(bytes))) return false;
    started_ = true;
    accounted_ += size;
    ++frames_;
    return true;
}

void KinectDepthRecordFile::record_write(std::string& bytes) {
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
        std::filesystem::path where = std::filesystem::absolute(std::filesystem::path(path_), error).parent_path();
        if (!error) {
            const std::filesystem::space_info space = std::filesystem::space(where, error);
            if (!error) free_bytes = space.available;
        }
        std::cerr << "NATIVE_KINECT_RECORD writing " << path_ << " (depth " << width_ << 'x' << height_ << ", at most "
                  << limit_ / (1024 * 1024) << " MB, "
                  << (every_ == 1 ? std::string("every image") : "1 image in " + std::to_string(every_));
        if (free_bytes != UINT64_MAX) std::cerr << ", " << free_bytes / (1024 * 1024) << " MB free";
        std::cerr << ')' << std::endl;
        if (free_bytes != UINT64_MAX) {
            const std::string warning = kinect_depth_space_warning(path_, free_bytes, limit_);
            if (!warning.empty()) std::cerr << warning << std::endl;
        }
    }
    if (file_) std::fwrite(bytes.data(), 1, bytes.size(), file_);
}

void KinectDepthRecordFile::record_flush() {
    if (file_) std::fflush(file_);
}

namespace {
std::mutex& depth_lock() {
    static std::mutex lock;
    return lock;
}
bool& depth_switch() {
    static bool on = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_DEPTH");
        return text && *text == '1';
    }();
    return on;
}
}

bool kinect_record_depth_enabled() {
    std::lock_guard guard(depth_lock());
    return depth_switch();
}

void kinect_record_depth_enable() {
    std::lock_guard guard(depth_lock());
    depth_switch() = true;
}

uint64_t kinect_record_depth_limit_bytes() {
    static const uint64_t limit = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_DEPTH_MB");
        const double mb = text && *text ? std::strtod(text, nullptr) : 4096.0;
        return uint64_t((mb >= 1 && mb <= 1048576 ? mb : 4096.0) * 1024 * 1024);
    }();
    return limit;
}

uint32_t kinect_record_depth_every() {
    static const uint32_t every = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_DEPTH_EVERY");
        const long value = text && *text ? std::strtol(text, nullptr, 10) : 1;
        return uint32_t(value >= 1 && value <= 1000 ? value : 1);
    }();
    return every;
}

std::string kinect_record_depth_path(const std::string& prefix, const std::string& tag) {
    return tag.empty() ? prefix + "-depth.bin" : prefix + "-" + tag + "-depth.bin";
}

KinectDepthRecordFile* kinect_record_depth_file(const std::string& tag) {
    if (!kinect_record_depth_enabled()) return nullptr;
    const std::string& prefix = kinect_record_prefix();
    if (prefix.empty()) return nullptr;
    std::lock_guard guard(depth_lock());
    static std::map<std::string, std::unique_ptr<KinectDepthRecordFile>> files;
    auto& file = files[tag];
    if (!file)
        file = std::make_unique<KinectDepthRecordFile>(kinect_record_depth_path(prefix, tag), kinect_record_depth_limit_bytes(),
                                                       kinect_record_depth_every());
    return file.get();
}

}
