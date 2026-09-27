#include "camera_player.h"

#include "camera_capture.h"
#include "pose_estimator.h"
#include "pose_smoothing.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace sfr {

struct CameraPlayer::Impl {
    std::unique_ptr<CameraCapture> camera;
    std::unique_ptr<PoseEstimator> estimator;
    PoseSmoothing smoothing = PoseSmoothing::from_environment();
    bool mirrored = false;
    std::mutex lock;
    SkeletonJoints joints{};
    uint64_t found = 0, taken = 0;
    std::atomic<bool> ever_found{false};
    // steady_clock nanoseconds of the last body, for left().
    std::atomic<int64_t> last_found{0};
    double leave_seconds = [] {
        const char* text = std::getenv("SFR_CAMERA_LEAVE_SECONDS");
        if (!text || !*text) return 1.5;
        const double value = std::strtod(text, nullptr);
        return value > 0 ? value : 0.0;
    }();
    std::jthread worker;

    void run(std::stop_token stop) {
        CameraFrame frame;
        PoseLandmarks landmarks{};
        SkeletonJoints mapped{};
        uint64_t estimates = 0;
        auto reported = std::chrono::steady_clock::now();
        auto last_picture = reported;
        double spent = 0;
        PoseMapping mapping;  // the scale and leading side, carried along (pose_to_joints)
        while (!stop.stop_requested()) {
            if (!camera->next(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!estimator) continue;  // a picture for the title, no body
            const auto started = std::chrono::steady_clock::now();
            const double interval = std::chrono::duration<double>(started - last_picture).count();
            last_picture = started;
            bool body = estimator->estimate(frame, landmarks);
            if (body) {
                // Smoothed where the model read them, before the picture
                // becomes metres: the wandering is a picture's wandering.
                smoothing.smooth(landmarks, interval);
                body = pose_to_joints(landmarks, frame.width, frame.height, mapped, mirrored, &mapping);
            } else {
                smoothing.forget();
                mapping = {};  // whoever comes next is measured afresh
            }
            spent += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            ++estimates;
            if (body) {
                std::lock_guard guard(lock);
                joints = mapped;
                ++found;
                ever_found.store(true, std::memory_order_relaxed);
                last_found.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_relaxed);
            }
            // Once every five seconds: how well the camera is keeping up.
            const auto now = std::chrono::steady_clock::now();
            if (now - reported >= std::chrono::seconds(5)) {
                std::cerr << "NATIVE_CAMERA_PLAYER estimates=" << estimates << " tracked=" << found
                          << " average_ms=" << (estimates ? spent / double(estimates) : 0.0) << '\n';
                reported = now;
                estimates = 0;
                spent = 0;
            }
        }
    }
};

CameraPlayer::CameraPlayer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraPlayer::~CameraPlayer() = default;

std::unique_ptr<CameraPlayer> CameraPlayer::start() {
    const char* const setting = std::getenv("SFR_CAMERA");
    const std::string choice = setting ? setting : "";
    if (choice.empty() || choice == "off" || choice == "0") return nullptr;
    const bool motion = choice == "motion";

    auto impl = std::make_unique<Impl>();
    impl->camera = CameraCapture::open(640, 480);
    if (!impl->camera) return nullptr;
    if (motion) {
        impl->estimator = PoseEstimator::open(PoseEstimator::default_model());
        if (!impl->estimator) {
            std::cerr << "NATIVE_CAMERA_PLAYER motion=0 reason=no-pose-model\n";
            return nullptr;
        }
    }
    // SFR_CAMERA_MIRROR=1 for a camera that hands over a mirrored picture,
    // which the phone-as-webcam apps tend to do by default.
    if (const char* text = std::getenv("SFR_CAMERA_MIRROR"); text && *text && *text != '0') impl->mirrored = true;
    Impl* const raw = impl.get();
    impl->worker = std::jthread([raw](std::stop_token stop) { raw->run(stop); });
    std::cerr << "NATIVE_CAMERA_PLAYER started motion=" << motion << " mirrored=" << raw->mirrored << '\n';
    return std::unique_ptr<CameraPlayer>(new CameraPlayer(std::move(impl)));
}

bool CameraPlayer::joints(SkeletonJoints& out) {
    std::lock_guard guard(impl_->lock);
    if (impl_->found == impl_->taken) return false;
    out = impl_->joints;
    impl_->taken = impl_->found;
    return true;
}

bool CameraPlayer::tracking() const { return impl_->ever_found.load(std::memory_order_relaxed); }

bool CameraPlayer::left_after(double seconds_without_body, double leave_seconds) {
    return leave_seconds > 0 && seconds_without_body >= leave_seconds;
}

bool CameraPlayer::left() const {
    if (!tracking()) return false;
    const auto last = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(impl_->last_found.load(std::memory_order_relaxed)));
    const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count();
    return left_after(since, impl_->leave_seconds);
}

}
