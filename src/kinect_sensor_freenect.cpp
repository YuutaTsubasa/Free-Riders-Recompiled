// A Kinect v1 on macOS through libfreenect (#305). There is no Kinect for
// Windows runtime there, so what it would make of the cameras is made here
// (kinect_depth_body.h): the skeleton from the colour picture's pose and the
// depth, the depth image's player index grown from that skeleton. One
// player: the pose model finds one body.
//
// Two threads: libfreenect's events, which hand over each depth picture
// (millimetres, the depth camera's view, 640x480 halved to the game's
// 320x240) and colour picture (RGB 640x480); and the tracker, which runs the
// pose model on each new colour picture and makes each new depth picture the
// game's, with the player cut out by the newest body.
//
// libfreenect's pictures are taken to be mirrored as the runtime's are (the
// player's right hand on the picture's right). SFR_FREENECT_MIRROR=1 turns
// both pictures round for a sensor that hands them over the other way; which
// a Kinect v1 does through libfreenect has yet to be seen on hardware.
#include "kinect_depth_body.h"
#include "kinect_record.h"
#include "kinect_sensor.h"
#include "camera_capture.h"
#include "pose_estimator.h"

#include <libfreenect.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include <sys/time.h>

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

class FreenectKinect final : public KinectSensor {
public:
    FreenectKinect(freenect_context* context, freenect_device* device, bool motor,
                   std::unique_ptr<PoseEstimator> estimator)
        : context_(context), device_(device), motor_(motor), estimator_(std::move(estimator)) {
        freenect_set_user(device_, this);
        freenect_set_depth_callback(device_, [](freenect_device* device, void* data, uint32_t) {
            static_cast<FreenectKinect*>(freenect_get_user(device))->on_depth(static_cast<const uint16_t*>(data));
        });
        freenect_set_video_callback(device_, [](freenect_device* device, void* data, uint32_t) {
            static_cast<FreenectKinect*>(freenect_get_user(device))->on_colour(static_cast<const uint8_t*>(data));
        });
        freenect_start_depth(device_);
        freenect_start_video(device_);
        events_ = std::thread([this] {
            while (!stopping_) {
                timeval wait{0, 100000};
                if (freenect_process_events_timeout(context_, &wait) < 0) {
                    std::cerr << "NATIVE_KINECT model=v1 backend=libfreenect events=failed" << std::endl;
                    break;
                }
            }
        });
        tracker_ = std::thread([this] { track(); });
        std::cerr << "NATIVE_KINECT started=1 model=v1 backend=libfreenect motor=" << motor_
                  << " pose=" << (estimator_ ? 1 : 0) << " mirrored=" << mirror_pictures() << std::endl;
    }

    ~FreenectKinect() override {
        stopping_ = true;
        arrived_.notify_all();
        if (tracker_.joinable()) tracker_.join();
        if (events_.joinable()) events_.join();
        freenect_stop_depth(device_);
        freenect_stop_video(device_);
        freenect_close_device(device_);
        freenect_shutdown(context_);
    }

    const char* model() const override { return "Kinect v1"; }
    bool has_depth_stream() const override { return true; }
    bool has_colour_stream() const override { return true; }

    bool image(KinectImageKind kind, KinectImage& out) override {
        std::lock_guard guard(images_lock_);
        const KinectImage& latest = kind == KinectImageKind::colour ? colour_image_ : depth_image_;
        if (latest.number == 0 || latest.number == out.number) return false;
        out = latest;
        return true;
    }

    bool next(KinectFrame& frame) override {
        std::lock_guard guard(frame_lock_);
        if (frame_.number == 0 || frame_.number == frame.number) return false;
        frame = frame_;
        return true;
    }

    bool elevation(int& degrees) override {
        if (!motor_) return false;
        std::lock_guard guard(motor_lock_);
        if (freenect_update_tilt_state(device_) < 0) return false;
        degrees = int(std::lround(freenect_get_tilt_degs(freenect_get_tilt_state(device_))));
        return true;
    }

    bool set_elevation(int degrees) override {
        if (!motor_) return false;
        std::lock_guard guard(motor_lock_);
        return freenect_set_tilt_degs(device_, std::clamp(degrees, -27, 27)) >= 0;
    }

private:
    // libfreenect's thread: copy and go.
    void on_depth(const uint16_t* depth) {
        std::lock_guard guard(arrival_lock_);
        if (mirror_pictures()) {
            depth_raw_.resize(640 * 480);
            for (int y = 0; y < 480; ++y)
                for (int x = 0; x < 640; ++x) depth_raw_[size_t(y) * 640 + x] = depth[size_t(y) * 640 + (639 - x)];
            kinect_depth_halve(depth_raw_.data(), depth_new_);
        } else {
            kinect_depth_halve(depth, depth_new_);
        }
        depth_steady_us_ = kinect_record_now_us();
        ++depth_arrived_;
        arrived_.notify_one();
    }

    void on_colour(const uint8_t* rgb) {
        std::lock_guard guard(arrival_lock_);
        colour_new_.width = 640;
        colour_new_.height = 480;
        colour_new_.bgra.resize(640 * 480 * 4);
        for (int y = 0; y < 480; ++y)
            for (int x = 0; x < 640; ++x) {
                const uint8_t* in = rgb + (size_t(y) * 640 + size_t(mirror_pictures() ? 639 - x : x)) * 3;
                uint8_t* out = &colour_new_.bgra[(size_t(y) * 640 + x) * 4];
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = 255;
            }
        colour_steady_us_ = kinect_record_now_us();
        ++colour_arrived_;
        arrived_.notify_one();
    }

    // The tracker's thread.
    void track() {
        uint64_t depth_seen = 0, colour_seen = 0;
        KinectDepthMillimetres depth;
        CameraFrame colour;
        std::vector<uint8_t> index;
        SkeletonJoints joints{};
        std::array<uint32_t, nui_joint_count> states{};
        bool have_body = false;
        clock::time_point body_at{};
        uint32_t tracking_id = 0, next_id = 1;
        uint64_t poses = 0, bodies = 0;
        double pose_ms = 0;
        clock::time_point reported = clock::now();
        while (!stopping_) {
            bool new_depth = false, new_colour = false;
            int64_t depth_us = 0, colour_us = 0;
            {
                std::unique_lock lock(arrival_lock_);
                arrived_.wait_for(lock, std::chrono::milliseconds(200), [&] {
                    return stopping_ || depth_arrived_ != depth_seen || colour_arrived_ != colour_seen;
                });
                if (stopping_) break;
                if (depth_arrived_ != depth_seen) {
                    depth_seen = depth_arrived_;
                    depth = depth_new_;
                    depth_us = depth_steady_us_;
                    new_depth = true;
                }
                if (colour_arrived_ != colour_seen) {
                    colour_seen = colour_arrived_;
                    colour = colour_new_;
                    colour_us = colour_steady_us_;
                    new_colour = true;
                }
            }
            if (new_colour) {
                {
                    std::lock_guard guard(images_lock_);
                    colour_image_.width = 640;
                    colour_image_.height = 480;
                    colour_image_.bytes_per_pixel = 4;
                    colour_image_.pixels = colour.bgra;
                    colour_image_.steady_us = colour_us;
                    ++colour_image_.number;
                }
                // The pose, placed by the newest depth.
                if (estimator_ && !depth.empty()) {
                    const auto started = clock::now();
                    PoseLandmarks landmarks{};
                    const bool found = estimator_->estimate(colour, landmarks) &&
                                       kinect_depth_body(landmarks, depth, {}, joints, states);
                    pose_ms += std::chrono::duration<double, std::milli>(clock::now() - started).count();
                    ++poses;
                    if (found) {
                        if (!have_body) tracking_id = next_id++;
                        have_body = true;
                        body_at = clock::now();
                        ++bodies;
                    }
                }
                // A body not seen for half a second has left.
                if (have_body && clock::now() - body_at > std::chrono::milliseconds(500)) have_body = false;
                publish_frame(have_body, tracking_id, joints, states);
            }
            if (new_depth) {
                index.assign(depth.size(), 0);
                if (have_body) kinect_cut_player(depth, joints, 1, {}, index);
                std::lock_guard guard(images_lock_);
                depth_image_.width = kinect_depth_width;
                depth_image_.height = kinect_depth_height;
                depth_image_.bytes_per_pixel = 2;
                kinect_depth_and_player(depth, index, depth_image_.pixels);
                depth_image_.steady_us = depth_us;
                ++depth_image_.number;
            }
            if (clock::now() - reported >= std::chrono::seconds(5)) {
                reported = clock::now();
                std::cerr << "NATIVE_KINECT backend=libfreenect depth=" << depth_seen << " colour=" << colour_seen
                          << " poses=" << poses << " bodies=" << bodies
                          << " pose_ms=" << (poses ? pose_ms / double(poses) : 0.0) << std::endl;
                poses = bodies = 0;
                pose_ms = 0;
            }
        }
    }

    void publish_frame(bool have_body, uint32_t tracking_id, const SkeletonJoints& joints,
                       const std::array<uint32_t, nui_joint_count>& states) {
        std::lock_guard guard(frame_lock_);
        frame_.captured_at = clock::now();
        frame_.bodies.clear();
        if (have_body) {
            KinectBody body;
            body.tracking_id = tracking_id;
            body.sensor_index = 0;  // its pixels carry player index 1
            body.joints = joints;
            body.joint_states = states;
            body.position = joints[nui_joint::hip_center];
            frame_.bodies.push_back(body);
        }
        ++frame_.number;
    }

    freenect_context* context_;
    freenect_device* device_;
    const bool motor_;
    std::unique_ptr<PoseEstimator> estimator_;
    std::atomic<bool> stopping_{false};
    std::thread events_, tracker_;
    std::mutex motor_lock_;

    std::mutex arrival_lock_;
    std::condition_variable arrived_;
    std::vector<uint16_t> depth_raw_;
    KinectDepthMillimetres depth_new_;
    CameraFrame colour_new_;
    int64_t depth_steady_us_ = 0, colour_steady_us_ = 0;
    uint64_t depth_arrived_ = 0, colour_arrived_ = 0;

    std::mutex images_lock_;
    KinectImage depth_image_, colour_image_;
    std::mutex frame_lock_;
    KinectFrame frame_;
};

std::unique_ptr<KinectSensor> open_freenect(std::string* why) {
    const auto fail = [&](const char* reason) -> std::unique_ptr<KinectSensor> {
        if (why) *why = reason;
        return nullptr;
    };
    freenect_context* context = nullptr;
    if (freenect_init(&context, nullptr) < 0) return fail("no-runtime");
    freenect_set_log_level(context, FREENECT_LOG_WARNING);
    // The motor and accelerometer too where the sensor lets them be opened
    // (a model 1473 wants firmware for them, which is not uploaded).
    bool motor = true;
    freenect_select_subdevices(context, freenect_device_flags(FREENECT_DEVICE_CAMERA | FREENECT_DEVICE_MOTOR));
    if (freenect_num_devices(context) < 1) {
        freenect_shutdown(context);
        return fail("no-sensor");
    }
    freenect_device* device = nullptr;
    if (freenect_open_device(context, &device, 0) < 0) {
        motor = false;
        freenect_select_subdevices(context, FREENECT_DEVICE_CAMERA);
        if (freenect_open_device(context, &device, 0) < 0) {
            freenect_shutdown(context);
            return fail("start-failed");
        }
    }
    const freenect_frame_mode depth_mode = freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_MM);
    const freenect_frame_mode colour_mode = freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_RGB);
    if (!depth_mode.is_valid || !colour_mode.is_valid || freenect_set_depth_mode(device, depth_mode) < 0 ||
        freenect_set_video_mode(device, colour_mode) < 0) {
        freenect_close_device(device);
        freenect_shutdown(context);
        return fail("start-failed");
    }
    // The skeleton comes from the camera's pose model; without it the
    // sensor still gives the game its pictures, with nobody in them.
    auto estimator = PoseEstimator::open(PoseEstimator::default_model());
    if (!estimator) std::cerr << "NATIVE_KINECT model=v1 backend=libfreenect pose=unavailable" << std::endl;
    return std::make_unique<FreenectKinect>(context, device, motor, std::move(estimator));
}
}

// A Kinect v2 through libfreenect2 (kinect_sensor_freenect2.cpp).
std::unique_ptr<KinectSensor> open_freenect2(std::string* why);

bool KinectSensor::supported() { return true; }

std::unique_ptr<KinectSensor> KinectSensor::open(std::string* why) {
    // The Xbox 360 kind first, as on Windows.
    std::string v1_why, v2_why;
    if (auto sensor = open_freenect(&v1_why)) return sensor;
    if (auto sensor = open_freenect2(&v2_why)) return sensor;
    if (why) *why = kinect_open_failure(v1_why, v2_why);
    return nullptr;
}

std::unique_ptr<KinectSensor> KinectSensor::open_model(int model, std::string* why) {
    if (model == 1) return open_freenect(why);
    if (model == 2) return open_freenect2(why);
    if (why) *why = "unknown-model";
    return nullptr;
}

}
