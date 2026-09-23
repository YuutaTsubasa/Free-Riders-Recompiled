// The webcam on Linux, through V4L2 with memory-mapped buffers. BGRA is
// asked for first (few cameras offer it), then YUY2, then NV12, which
// camera_capture.cpp converts.
#include "camera_capture.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace sfr {
namespace {
int retry_ioctl(int descriptor, unsigned long request, void* argument) {
    int result = 0;
    do { result = ioctl(descriptor, request, argument); } while (result == -1 && errno == EINTR);
    return result;
}

class V4L2Camera final : public CameraCapture {
public:
    struct Buffer { void* start = nullptr; size_t length = 0; };

    V4L2Camera(int descriptor, std::vector<Buffer> buffers, CameraPixels format, uint32_t width, uint32_t height,
               uint32_t stride)
        : descriptor_(descriptor), buffers_(std::move(buffers)), format_(format), width_(width), height_(height),
          stride_(stride) {}

    ~V4L2Camera() override {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        retry_ioctl(descriptor_, VIDIOC_STREAMOFF, &type);
        for (const Buffer& buffer : buffers_) munmap(buffer.start, buffer.length);
        close(descriptor_);
    }

    bool next(CameraFrame& frame) override {
        // Non-blocking: no picture yet leaves the last one in place.
        v4l2_buffer taken{};
        taken.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        taken.memory = V4L2_MEMORY_MMAP;
        if (retry_ioctl(descriptor_, VIDIOC_DQBUF, &taken) == -1) return false;
        bool converted = false;
        if (taken.index < buffers_.size()) {
            const Buffer& buffer = buffers_[taken.index];
            const size_t used = taken.bytesused ? taken.bytesused : buffer.length;
            converted = convert_camera_pixels(format_,
                                              {static_cast<const uint8_t*>(buffer.start), used},
                                              width_, height_, stride_, frame);
        }
        retry_ioctl(descriptor_, VIDIOC_QBUF, &taken);
        if (converted) frame.number = ++number_;
        return converted;
    }

private:
    int descriptor_;
    std::vector<Buffer> buffers_;
    CameraPixels format_;
    uint32_t width_, height_, stride_;
    uint64_t number_ = 0;
};
}

std::unique_ptr<CameraCapture> CameraCapture::open(uint32_t width, uint32_t height) {
    // SFR_CAMERA_DEVICE=N reads /dev/videoN instead of /dev/video0.
    std::string path = "/dev/video0";
    if (const char* text = std::getenv("SFR_CAMERA_DEVICE"); text && *text) path = "/dev/video" + std::string(text);
    const int descriptor = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (descriptor == -1) {
        std::cerr << "NATIVE_CAMERA unavailable=no-device path=" << path << '\n';
        return nullptr;
    }
    const auto give_up = [&](const char* why) -> std::unique_ptr<CameraCapture> {
        std::cerr << "NATIVE_CAMERA unavailable=" << why << '\n';
        close(descriptor);
        return nullptr;
    };

    v4l2_capability capability{};
    if (retry_ioctl(descriptor, VIDIOC_QUERYCAP, &capability) == -1 ||
        !(capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(capability.capabilities & V4L2_CAP_STREAMING))
        return give_up("not-a-streaming-camera");

    CameraPixels format = CameraPixels::bgra;
    v4l2_format wanted{};
    bool settled = false;
    for (const auto& [fourcc, pixels] : {std::pair{V4L2_PIX_FMT_ABGR32, CameraPixels::bgra},
                                         std::pair{V4L2_PIX_FMT_YUYV, CameraPixels::yuy2},
                                         std::pair{V4L2_PIX_FMT_NV12, CameraPixels::nv12}}) {
        wanted = {};
        wanted.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        wanted.fmt.pix.width = width;
        wanted.fmt.pix.height = height;
        wanted.fmt.pix.pixelformat = fourcc;
        wanted.fmt.pix.field = V4L2_FIELD_NONE;
        if (retry_ioctl(descriptor, VIDIOC_S_FMT, &wanted) == -1) continue;
        if (wanted.fmt.pix.pixelformat != fourcc) continue;  // the driver chose another
        format = pixels;
        settled = true;
        break;
    }
    if (!settled) return give_up("unsupported-format");

    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (retry_ioctl(descriptor, VIDIOC_REQBUFS, &request) == -1 || request.count < 2)
        return give_up("no-buffers");

    std::vector<V4L2Camera::Buffer> buffers;
    for (uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer description{};
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        description.memory = V4L2_MEMORY_MMAP;
        description.index = i;
        if (retry_ioctl(descriptor, VIDIOC_QUERYBUF, &description) == -1) return give_up("buffer-query");
        void* start = mmap(nullptr, description.length, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor,
                           description.m.offset);
        if (start == MAP_FAILED) return give_up("buffer-map");
        buffers.push_back({start, description.length});
        if (retry_ioctl(descriptor, VIDIOC_QBUF, &description) == -1) return give_up("buffer-queue");
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (retry_ioctl(descriptor, VIDIOC_STREAMON, &type) == -1) return give_up("stream-on");

    std::cerr << "NATIVE_CAMERA device=\"" << path << "\" size=" << wanted.fmt.pix.width << 'x'
              << wanted.fmt.pix.height << " format="
              << (format == CameraPixels::bgra ? "bgra" : format == CameraPixels::yuy2 ? "yuy2" : "nv12")
              << " stride=" << wanted.fmt.pix.bytesperline << '\n';
    return std::make_unique<V4L2Camera>(descriptor, std::move(buffers), format, wanted.fmt.pix.width,
                                        wanted.fmt.pix.height, wanted.fmt.pix.bytesperline);
}
}
