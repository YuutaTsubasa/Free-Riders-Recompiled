#include "kinect_record_colour.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>

namespace sfr {

namespace {
constexpr char file_magic[8] = {'S', 'F', 'R', 'K', 'C', 'O', 'L', '1'};
constexpr char frame_magic[4] = {'K', 'C', 'F', '1'};

void put32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out += char(uint8_t(value >> (8 * i)));
}
void put64(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out += char(uint8_t(value >> (8 * i)));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }
}

bool kinect_colour_frame_pixels(KinectColourRecordFrame& frame, const uint8_t* bgrx, uint32_t width, uint32_t height,
                                uint32_t bytes_per_pixel) {
    if (bytes_per_pixel != 4 || !width || !height || !bgrx) return false;
    frame.width = width;
    frame.height = height;
    const size_t count = size_t(width) * height;
    frame.bgr.resize(count * 3);
    for (size_t i = 0; i < count; ++i) std::memcpy(&frame.bgr[3 * i], &bgrx[4 * i], 3);
    return true;
}

std::string kinect_colour_file_header(uint32_t width, uint32_t height) {
    std::string out(file_magic, 8);
    put32(out, kinect_colour_file_version);
    put32(out, kinect_colour_file_header_size);
    put32(out, width);
    put32(out, height);
    put32(out, kinect_colour_pixel_format_bgr);
    put32(out, kinect_colour_frame_header_size);
    return out;
}

std::string kinect_colour_encode_frame(const KinectColourRecordFrame& frame) {
    std::string out(frame_magic, 4);
    put32(out, uint32_t(frame.bgr.size()));
    put64(out, uint64_t(frame.steady_us));
    put64(out, uint64_t(frame.handed_us));
    put64(out, uint64_t(frame.sdk_time));
    put64(out, frame.number);
    out.append(reinterpret_cast<const char*>(frame.bgr.data()), frame.bgr.size());
    return out;
}

KinectColourReader::~KinectColourReader() {
    if (file_) std::fclose(file_);
}

bool KinectColourReader::open(const std::string& path) {
    if (file_) std::fclose(file_);
    file_ = std::fopen(path.c_str(), "rb");
    error_.clear();
    truncated_ = false;
    if (!file_) { error_ = "cannot open " + path; return false; }
    uint8_t header[kinect_colour_file_header_size];
    const size_t got = std::fread(header, 1, sizeof header, file_);
    const auto fail = [&](const char* why) {
        error_ = why;
        std::fclose(file_);
        file_ = nullptr;
        return false;
    };
    if (got < sizeof header || std::memcmp(header, file_magic, 8) != 0) return fail("not a colour recording");
    if (get32(header + 8) != kinect_colour_file_version || get32(header + 12) != kinect_colour_file_header_size)
        return fail("unknown version");
    width_ = get32(header + 16);
    height_ = get32(header + 20);
    if (get32(header + 24) != kinect_colour_pixel_format_bgr) return fail("unknown pixel format");
    frame_header_ = get32(header + 28);
    if (!width_ || !height_ || width_ > 4096 || height_ > 4096) return fail("bad image size");
    if (frame_header_ < kinect_colour_frame_header_size || frame_header_ > 4096) return fail("bad frame header size");
    return true;
}

bool KinectColourReader::next(KinectColourRecordFrame& frame) {
    if (!file_) return false;
    std::vector<uint8_t> header(frame_header_);
    const size_t got = std::fread(header.data(), 1, header.size(), file_);
    if (got == 0) return false;
    if (got < header.size()) { truncated_ = true; return false; }
    if (std::memcmp(header.data(), frame_magic, 4) != 0) { error_ = "a broken frame header"; return false; }
    const uint32_t bytes = get32(&header[4]);
    if (bytes != size_t(width_) * height_ * 3) { error_ = "a broken frame size"; return false; }
    frame.steady_us = int64_t(get64(&header[8]));
    frame.handed_us = int64_t(get64(&header[16]));
    frame.sdk_time = int64_t(get64(&header[24]));
    frame.number = get64(&header[32]);
    frame.width = width_;
    frame.height = height_;
    frame.bgr.resize(bytes);
    if (std::fread(frame.bgr.data(), 1, bytes, file_) < bytes) { truncated_ = true; return false; }
    return true;
}

KinectColourRecordFile::KinectColourRecordFile(std::string path, uint64_t limit_bytes, uint32_t every,
                                               KinectRecordQueue* queue)
    : queue_(queue ? *queue : KinectRecordQueue::process()), path_(std::move(path)), limit_(limit_bytes),
      every_(every ? every : 1) {}

KinectColourRecordFile::~KinectColourRecordFile() {
    queue_.drain(this, std::chrono::seconds(10));
    if (file_) std::fclose(file_);
}

uint64_t KinectColourRecordFile::frames_written() const {
    std::lock_guard guard(lock_);
    return frames_;
}

bool KinectColourRecordFile::write(const KinectColourRecordFrame& frame) {
    std::lock_guard guard(lock_);
    if (failed_ || full_) return false;
    if (offered_++ % every_) return false;
    if (!started_) {
        width_ = frame.width;
        height_ = frame.height;
    }
    if (frame.width != width_ || frame.height != height_ || frame.bgr.size() != size_t(width_) * height_ * 3) return false;
    std::string bytes = started_ ? std::string() : kinect_colour_file_header(width_, height_);
    bytes += kinect_colour_encode_frame(frame);
    if (accounted_ + bytes.size() > limit_) {
        full_ = true;
        std::cerr << "NATIVE_KINECT_RECORD colour full " << path_ << " after " << accounted_ << " bytes (" << frames_
                  << " pictures): stopped" << std::endl;
        return false;
    }
    const uint64_t size = bytes.size();
    if (!queue_.push(this, std::move(bytes))) return false;
    started_ = true;
    accounted_ += size;
    ++frames_;
    return true;
}

void KinectColourRecordFile::record_write(std::string& bytes) {
    if (!opened_) {
        opened_ = true;
        file_ = std::fopen(path_.c_str(), "wb");
        if (!file_) {
            failed_ = true;
            std::cerr << "NATIVE_KINECT_RECORD failed to open " << path_ << std::endl;
            return;
        }
        std::cerr << "NATIVE_KINECT_RECORD writing " << path_ << " (colour " << width_ << 'x' << height_ << ", at most "
                  << limit_ / (1024 * 1024) << " MB, "
                  << (every_ == 1 ? std::string("every picture") : "1 picture in " + std::to_string(every_));
        std::error_code error;
        const auto where = std::filesystem::absolute(std::filesystem::path(path_), error).parent_path();
        if (!error)
            if (const auto space = std::filesystem::space(where, error); !error)
                std::cerr << ", " << space.available / (1024 * 1024) << " MB free";
        std::cerr << ')' << std::endl;
    }
    if (file_) std::fwrite(bytes.data(), 1, bytes.size(), file_);
}

void KinectColourRecordFile::record_flush() {
    if (file_) std::fflush(file_);
}

namespace {
std::mutex& colour_lock() {
    static std::mutex lock;
    return lock;
}
bool& colour_switch() {
    static bool on = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_COLOUR");
        return text && *text == '1';
    }();
    return on;
}
}

bool kinect_record_colour_enabled() {
    std::lock_guard guard(colour_lock());
    return colour_switch();
}

void kinect_record_colour_enable() {
    std::lock_guard guard(colour_lock());
    colour_switch() = true;
}

uint64_t kinect_record_colour_limit_bytes() {
    static const uint64_t limit = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_COLOUR_MB");
        const double mb = text && *text ? std::strtod(text, nullptr) : 4096.0;
        return uint64_t((mb >= 1 && mb <= 1048576 ? mb : 4096.0) * 1024 * 1024);
    }();
    return limit;
}

uint32_t kinect_record_colour_every() {
    static const uint32_t every = [] {
        const char* text = std::getenv("SFR_KINECT_RECORD_COLOUR_EVERY");
        const long value = text && *text ? std::strtol(text, nullptr, 10) : 2;
        return uint32_t(value >= 1 && value <= 1000 ? value : 2);
    }();
    return every;
}

std::string kinect_record_colour_path(const std::string& prefix, const std::string& tag) {
    return tag.empty() ? prefix + "-colour.bin" : prefix + "-" + tag + "-colour.bin";
}

KinectColourRecordFile* kinect_record_colour_file(const std::string& tag) {
    if (!kinect_record_colour_enabled()) return nullptr;
    const std::string& prefix = kinect_record_prefix();
    if (prefix.empty()) return nullptr;
    std::lock_guard guard(colour_lock());
    static std::map<std::string, std::unique_ptr<KinectColourRecordFile>> files;
    auto& file = files[tag];
    if (!file)
        file = std::make_unique<KinectColourRecordFile>(kinect_record_colour_path(prefix, tag),
                                                        kinect_record_colour_limit_bytes(), kinect_record_colour_every());
    return file.get();
}

}
