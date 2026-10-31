// A Kinect v2 on macOS through libfreenect2 (#305). Like the Kinect v1 there
// (kinect_sensor_freenect.cpp), there is no runtime to track bodies, so the
// skeleton and the player's pixels are made here (kinect_depth_body.h) on
// the v2's own depth camera: its pose from the colour picture, placed in
// metres by the depth, the player grown through the depth from it. One
// player: the pose model finds one body.
//
// From there it is what a Kinect v2 on Windows hands the game
// (kinect_v2_win32.cpp, docs/kinect-v2.md): the skeleton and the depth moved
// into a virtual v1 (KinectDepthViewer, kinect_v2_synthesize_depth), so the
// race reads a v1's 320x240 depth-and-player image.
//
// libfreenect2 decodes the depth on the CPU (its own threads) and the colour
// through VideoToolbox. Its registration gives the depth camera's lens with
// the distortion taken out, and for each depth pixel the colour pixel it
// falls on, from which the colour picture's landmarks find their depth
// (KinectColourDepthGrid). One thread here takes each pair of pictures, runs
// the pose model on the colour picture at half size, and makes the game's
// frame and image from them.
//
// The pictures are taken to be mirrored as the SDK's are (the player's right
// hand on the picture's right); SFR_FREENECT_MIRROR=1 turns both round, as
// for the v1. Which way libfreenect2 hands them over has yet to be seen on
// hardware.
#include "camera_capture.h"
#include "kinect_depth_body.h"
#include "kinect_record.h"
#include "kinect_sensor.h"
#include "kinect_v2_depth.h"
#include "kinect_v2_image.h"
#include "pose_estimator.h"

#include <libfreenect2/frame_listener_impl.h>
#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/logger.h>
#include <libfreenect2/packet_pipeline.h>
#include <libfreenect2/registration.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace sfr {
namespace {
using clock = std::chrono::steady_clock;

bool mirror_pictures() {
    static const bool mirror = [] {
        const char* text = std::getenv("SFR_FREENECT_MIRROR");
        return text && *text == '1';
    }();
    return mirror;
}

constexpr uint32_t colour_width = 1920, colour_height = 1080;
// The pose model's picture: the colour at half size.
constexpr uint32_t pose_width = colour_width / 2, pose_height = colour_height / 2;

// The depth camera with registration's lens, its pixels' centres where
// getPointXYZ puts them ((c + 0.5 - cx) / fx), y up; turned round with the
// pictures.
KinectDepthCamera depth_camera(const libfreenect2::Freenect2Device::IrCameraParams& ir, bool flip) {
    KinectDepthCamera camera;
    camera.width = kinect_v2_depth_width;
    camera.height = kinect_v2_depth_height;
    camera.focal_x = ir.fx;
    camera.focal_y = ir.fy;
    camera.centre_x = flip ? float(kinect_v2_depth_width) - 0.5f - ir.cx : ir.cx - 0.5f;
    camera.centre_y = ir.cy - 0.5f;
    return camera;
}

class FreenectKinectV2 final : public KinectSensor {
public:
    FreenectKinectV2(std::unique_ptr<libfreenect2::Freenect2> freenect, libfreenect2::Freenect2Device* device,
                     std::unique_ptr<PoseEstimator> estimator)
        : freenect_(std::move(freenect)), device_(device), estimator_(std::move(estimator)),
          listener_(libfreenect2::Frame::Color | libfreenect2::Frame::Depth) {
        device_->setColorFrameListener(&listener_);
        device_->setIrAndDepthFrameListener(&listener_);
    }

    bool start(std::string* why) {
        if (!device_->start()) {
            if (why) *why = "start-failed";
            return false;
        }
        const auto ir = device_->getIrCameraParams();
        registration_ = std::make_unique<libfreenect2::Registration>(ir, device_->getColorCameraParams());
        camera_ = depth_camera(ir, mirror_pictures());
        if (viewer_.settings().mode == KinectDepthViewMode::fit && !(viewer_.settings().fit_scale > 0))
            viewer_.set_fit_scale(kinect_v2_fit_scale(ir.fy));
        std::cerr << "NATIVE_KINECT started=1 model=v2 backend=libfreenect2 serial=" << device_->getSerialNumber()
                  << " firmware=" << device_->getFirmwareVersion() << " pose=" << (estimator_ ? 1 : 0)
                  << " mirrored=" << mirror_pictures() << " lens=" << ir.fx << ',' << ir.fy << ',' << ir.cx << ','
                  << ir.cy << " view=" << viewer_.settings().view_name() << std::endl;
        worker_ = std::thread([this] { run(); });
        return true;
    }

    ~FreenectKinectV2() override {
        stopping_ = true;
        if (worker_.joinable()) worker_.join();
        device_->stop();
        device_->close();
        // freenect_ closes after the device it opened.
    }

    const char* model() const override { return "Kinect v2"; }
    bool has_depth_stream() const override { return true; }

    bool game_image(KinectImageKind kind, KinectImage& out) override {
        if (kind != KinectImageKind::depth_and_player) return false;
        std::lock_guard guard(image_lock_);
        if (game_image_.number == 0 || game_image_.number == out.number) return false;
        out = game_image_;
        return true;
    }

    bool next(KinectFrame& frame) override {
        std::lock_guard guard(frame_lock_);
        if (frame_.number == 0 || frame_.number == frame.number) return false;
        frame = frame_;
        return true;
    }

    void set_depth_settings(const KinectDepthSettings& settings) override {
        std::lock_guard guard(settings_lock_);
        pending_settings_ = settings;
    }

private:
    void apply_depth_settings() {
        std::optional<KinectDepthSettings> settings;
        {
            std::lock_guard guard(settings_lock_);
            settings.swap(pending_settings_);
        }
        if (!settings) return;
        const float fit = viewer_.fit_scale();
        viewer_.set_settings(*settings);
        if (settings->mode == KinectDepthViewMode::fit && !(settings->fit_scale > 0)) viewer_.set_fit_scale(fit);
    }

    // The colour picture at half size for the pose model (each 2x2's mean),
    // turned round when the pictures are.
    void pose_picture(const libfreenect2::Frame& rgb, CameraFrame& out) const {
        out.width = pose_width;
        out.height = pose_height;
        out.bgra.resize(size_t(pose_width) * pose_height * 4);
        const bool flip = mirror_pictures();
        for (uint32_t y = 0; y < pose_height; ++y)
            for (uint32_t x = 0; x < pose_width; ++x) {
                const uint32_t from_x = flip ? colour_width - 2 - 2 * x : 2 * x;
                const uint8_t* a = rgb.data + (size_t(2 * y) * colour_width + from_x) * 4;
                const uint8_t* b = a + size_t(colour_width) * 4;
                uint8_t* to = &out.bgra[(size_t(y) * pose_width + x) * 4];
                for (int c = 0; c < 3; ++c) to[c] = uint8_t((a[c] + a[4 + c] + b[c] + b[4 + c] + 2) / 4);
                to[3] = 255;
            }
        ++out.number;
    }

    // registration's undistorted depth (float millimetres) as whole ones,
    // turned round when the pictures are.
    void depth_millimetres(const libfreenect2::Frame& undistorted, KinectDepthMillimetres& out) const {
        const auto* mm = reinterpret_cast<const float*>(undistorted.data);
        out.resize(camera_.pixels());
        const bool flip = mirror_pictures();
        for (uint32_t y = 0; y < camera_.height; ++y)
            for (uint32_t x = 0; x < camera_.width; ++x) {
                const float value = mm[size_t(y) * camera_.width + (flip ? camera_.width - 1 - x : x)];
                out[size_t(y) * camera_.width + x] =
                    std::isfinite(value) && value > 0 ? uint16_t((std::min)(value + 0.5f, 65535.0f)) : 0;
            }
    }

    void run() {
        libfreenect2::Frame undistorted(kinect_v2_depth_width, kinect_v2_depth_height, 4);
        libfreenect2::Frame registered(kinect_v2_depth_width, kinect_v2_depth_height, 4);
        std::vector<int> colour_of_depth(camera_.pixels(), -1);
        KinectDepthMillimetres depth;
        KinectColourDepthGrid grid;
        CameraFrame colour;
        std::vector<uint8_t> index;
        std::vector<uint8_t> body_index;
        std::vector<float> points;
        SkeletonJoints joints{};
        std::array<uint32_t, nui_joint_count> states{};
        bool have_body = false;
        clock::time_point body_at{};
        uint32_t tracking_id = 0, next_id = 1;
        uint64_t pictures = 0, poses = 0, bodies = 0, players = 0;
        double pose_ms = 0, made_ms = 0;
        const auto began = clock::now();
        auto reported = began;
        while (!stopping_) {
            apply_depth_settings();
            libfreenect2::FrameMap frames;
            if (!listener_.waitForNewFrame(frames, 200)) continue;
            const libfreenect2::Frame* rgb = frames[libfreenect2::Frame::Color];
            const libfreenect2::Frame* raw_depth = frames[libfreenect2::Frame::Depth];
            const auto started = clock::now();
            const int64_t steady_us = kinect_record_now_us();
            const bool usable = rgb && raw_depth && rgb->width == colour_width && rgb->height == colour_height &&
                                rgb->bytes_per_pixel == 4 && raw_depth->width == kinect_v2_depth_width &&
                                raw_depth->height == kinect_v2_depth_height;
            if (usable) {
                registration_->apply(rgb, raw_depth, &undistorted, &registered, true, nullptr, colour_of_depth.data());
                depth_millimetres(undistorted, depth);
                grid.build(colour_of_depth.data(), depth, camera_, colour_width, colour_height, 4, mirror_pictures());
                pose_picture(*rgb, colour);
            }
            listener_.release(frames);
            if (!usable) continue;
            ++pictures;
            const double seconds = std::chrono::duration<double>(started - began).count();

            // The body: the pose at half size, its landmarks placed in the
            // depth through the colour picture's full size.
            if (estimator_) {
                const auto pose_started = clock::now();
                PoseLandmarks landmarks{};
                const auto to_depth = [&](float x, float y, float& depth_x, float& depth_y) {
                    return grid.find(x * 2.0f, y * 2.0f, depth_x, depth_y);
                };
                const bool found = estimator_->estimate(colour, landmarks) &&
                                   kinect_depth_body(landmarks, depth, camera_, to_depth, {}, joints, states);
                pose_ms += std::chrono::duration<double, std::milli>(clock::now() - pose_started).count();
                ++poses;
                if (found) {
                    if (!have_body) tracking_id = next_id++;
                    have_body = true;
                    body_at = clock::now();
                    ++bodies;
                }
            }
            if (have_body && clock::now() - body_at > std::chrono::milliseconds(500)) have_body = false;

            // The frame in the sensor's own space, for the viewer; then the
            // skeleton the game reads moved into the virtual v1.
            KinectFrame frame;
            frame.captured_at = started;
            if (have_body) {
                KinectBody body;
                body.tracking_id = tracking_id;
                body.sensor_index = 0;  // body index 0: player 1 in the made image
                body.joints = joints;
                body.joint_states = states;
                body.position = joints[nui_joint::hip_center];
                frame.bodies.push_back(body);
            }
            viewer_.body_frame(frame, seconds);
            const KinectDepthView skeleton_view = viewer_.skeleton_view();
            kinect_depth_view_frame(skeleton_view, frame);

            // The player's pixels, as the SDK's body index has them.
            index.assign(depth.size(), 0);
            if (have_body) players = kinect_cut_player(depth, camera_, joints, 1, {}, index);
            body_index.assign(depth.size(), kinect_v2_no_body);
            for (size_t i = 0; i < index.size(); ++i)
                if (index[i]) body_index[i] = 0;
            kinect_depth_camera_points(depth, camera_, points);
            const KinectDepthView view = viewer_.depth_frame(seconds);
            KinectImage made;
            const KinectDepthStats stats = kinect_v2_synthesize_depth(points.data(), body_index.data(), depth.size(), view, made);
            made.steady_us = steady_us;
            made.view_origin = view.origin;
            made.view_z_scale = view.z_scale;
            made.view_rotated = view.rotated;
            made.view_rotation = view.rotation;
            {
                std::lock_guard guard(image_lock_);
                made.number = game_image_.number + 1;
                game_image_ = std::move(made);
            }
            {
                std::lock_guard guard(frame_lock_);
                frame.number = frame_.number + 1;
                frame_ = std::move(frame);
            }
            made_ms += std::chrono::duration<double, std::milli>(clock::now() - started).count();

            if (clock::now() - reported >= std::chrono::seconds(5)) {
                reported = clock::now();
                std::cerr << "NATIVE_KINECT model=v2 backend=libfreenect2 frames=" << pictures << " poses=" << poses
                          << " bodies=" << bodies << " pose_ms=" << (poses ? pose_ms / double(poses) : 0.0)
                          << " frame_ms=" << (pictures ? made_ms / double(pictures) : 0.0)
                          << " depth_valid=" << double(stats.valid) / double(kinect_v1_depth_width * kinect_v1_depth_height)
                          << " depth_player_pixels=" << stats.players << " cut_pixels=" << players
                          << " depth_view=" << view.origin[0] << ',' << view.origin[1] << ',' << view.origin[2]
                          << std::endl;
                pictures = poses = bodies = 0;
                pose_ms = made_ms = 0;
            }
        }
    }

    std::unique_ptr<libfreenect2::Freenect2> freenect_;
    libfreenect2::Freenect2Device* device_;
    std::unique_ptr<PoseEstimator> estimator_;
    libfreenect2::SyncMultiFrameListener listener_;
    std::unique_ptr<libfreenect2::Registration> registration_;
    KinectDepthCamera camera_;
    KinectDepthViewer viewer_{KinectDepthSettings::from_environment()};  // the worker's
    std::mutex settings_lock_;
    std::optional<KinectDepthSettings> pending_settings_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex image_lock_;
    KinectImage game_image_;
    std::mutex frame_lock_;
    KinectFrame frame_;
};
}

std::unique_ptr<KinectSensor> open_freenect2(std::string* why) {
    const auto fail = [&](const char* reason) -> std::unique_ptr<KinectSensor> {
        if (why) *why = reason;
        return nullptr;
    };
    libfreenect2::setGlobalLogger(libfreenect2::createConsoleLogger(libfreenect2::Logger::Warning));
    auto freenect = std::make_unique<libfreenect2::Freenect2>();
    if (freenect->enumerateDevices() < 1) return fail("no-sensor");
    // The device takes the pipeline and deletes it.
    libfreenect2::Freenect2Device* device =
        freenect->openDevice(freenect->getDefaultDeviceSerialNumber(), new libfreenect2::CpuPacketPipeline());
    if (!device) return fail("start-failed");
    auto estimator = PoseEstimator::open(PoseEstimator::default_model());
    if (!estimator) std::cerr << "NATIVE_KINECT model=v2 backend=libfreenect2 pose=unavailable" << std::endl;
    auto sensor = std::make_unique<FreenectKinectV2>(std::move(freenect), device, std::move(estimator));
    if (!sensor->start(why)) return nullptr;
    return sensor;
}

}
