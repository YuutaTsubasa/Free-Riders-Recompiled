#include "camera_player.h"

#include "camera_capture.h"
#include "pose_estimator.h"

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
    std::mutex lock;
    SkeletonJoints joints{};
    uint64_t found = 0, taken = 0;
    std::atomic<bool> ever_found{false};
    std::jthread worker;

    void run(std::stop_token stop) {
        CameraFrame frame;
        PoseLandmarks landmarks{};
        SkeletonJoints mapped{};
        uint64_t estimates = 0;
        auto reported = std::chrono::steady_clock::now();
        double spent = 0;
        while (!stop.stop_requested()) {
            if (!camera->next(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!estimator) continue;  // a picture for the title, no body
            const auto started = std::chrono::steady_clock::now();
            const bool body = estimator->estimate(frame, landmarks) &&
                              pose_to_joints(landmarks, frame.width, frame.height, mapped);
            spent += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            ++estimates;
            if (body) {
                std::lock_guard guard(lock);
                joints = mapped;
                ++found;
                ever_found.store(true, std::memory_order_relaxed);
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
    Impl* const raw = impl.get();
    impl->worker = std::jthread([raw](std::stop_token stop) { raw->run(stop); });
    std::cerr << "NATIVE_CAMERA_PLAYER started motion=" << motion << '\n';
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

}
