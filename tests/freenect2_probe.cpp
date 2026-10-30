// Opens a Kinect v2 through libfreenect2 (macOS, #305) and counts what it
// delivers, so that a Mac's Kinect v2 can be checked without starting the
// game:
//   sfr_freenect2_probe [seconds]
// It prints the sensor's lens (registration's, which the game's skeleton and
// depth go through), and once a second the frames, how much of the depth is
// measured and the nearest point, and the colour picture's middle pixel.
#include <libfreenect2/frame_listener_impl.h>
#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/logger.h>
#include <libfreenect2/packet_pipeline.h>
#include <libfreenect2/registration.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    libfreenect2::setGlobalLogger(libfreenect2::createConsoleLogger(libfreenect2::Logger::Warning));
    libfreenect2::Freenect2 freenect;
    const int count = freenect.enumerateDevices();
    std::cout << "Kinect v2 devices: " << count << '\n';
    if (count < 1) return 1;
    libfreenect2::Freenect2Device* device =
        freenect.openDevice(freenect.getDefaultDeviceSerialNumber(), new libfreenect2::CpuPacketPipeline());
    if (!device) {
        std::cout << "could not open the Kinect v2\n";
        return 1;
    }
    libfreenect2::SyncMultiFrameListener listener(libfreenect2::Frame::Color | libfreenect2::Frame::Depth);
    device->setColorFrameListener(&listener);
    device->setIrAndDepthFrameListener(&listener);
    if (!device->start()) {
        std::cout << "the Kinect v2 would not start\n";
        device->close();
        return 1;
    }
    const auto ir = device->getIrCameraParams();
    const auto colour = device->getColorCameraParams();
    std::cout << "serial " << device->getSerialNumber() << " firmware " << device->getFirmwareVersion() << '\n'
              << "depth lens fx " << ir.fx << " fy " << ir.fy << " cx " << ir.cx << " cy " << ir.cy << " k1 " << ir.k1
              << " k2 " << ir.k2 << " k3 " << ir.k3 << '\n'
              << "colour lens fx " << colour.fx << " fy " << colour.fy << " cx " << colour.cx << " cy " << colour.cy
              << '\n';
    libfreenect2::Registration registration(ir, colour);
    libfreenect2::Frame undistorted(512, 424, 4), registered(512, 424, 4);
    std::cout << "reading for " << seconds << " s; stand in front of it\n";
    uint64_t frames = 0, timeouts = 0;
    const auto start = std::chrono::steady_clock::now(), end = start + std::chrono::seconds(seconds);
    auto shown = start;
    while (std::chrono::steady_clock::now() < end) {
        libfreenect2::FrameMap pictures;
        if (!listener.waitForNewFrame(pictures, 1000)) {
            ++timeouts;
            continue;
        }
        const libfreenect2::Frame* rgb = pictures[libfreenect2::Frame::Color];
        const libfreenect2::Frame* depth = pictures[libfreenect2::Frame::Depth];
        registration.apply(rgb, depth, &undistorted, &registered);
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - shown >= std::chrono::seconds(1)) {
            shown = now;
            const auto* mm = reinterpret_cast<const float*>(undistorted.data);
            uint32_t valid = 0;
            float nearest = 0;
            for (int i = 0; i < 512 * 424; ++i)
                if (std::isfinite(mm[i]) && mm[i] > 0) {
                    ++valid;
                    if (!nearest || mm[i] < nearest) nearest = mm[i];
                }
            const uint8_t* middle = rgb->data + (size_t(rgb->height / 2) * rgb->width + rgb->width / 2) * 4;
            std::cout << "frames " << frames << " colour " << rgb->width << 'x' << rgb->height << " valid "
                      << double(valid) / (512.0 * 424.0) << " nearest_mm " << nearest << " middle_bgr "
                      << int(middle[0]) << ',' << int(middle[1]) << ',' << int(middle[2]) << std::endl;
        }
        listener.release(pictures);
    }
    device->stop();
    device->close();
    std::cout << "frames: " << frames << ", waits without a frame: " << timeouts << '\n';
    return frames ? 0 : 1;
}
