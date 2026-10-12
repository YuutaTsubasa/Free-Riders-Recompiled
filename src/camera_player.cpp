#include "camera_player.h"

#include "camera_capture.h"
#include "pose_estimator.h"
#include "pose_smoothing.h"
#include "pose_stability.h"
#include "title_stats.h"

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
    PoseStabilizer stabilizer = PoseStabilizer::from_environment();
    MenuHandSteadying menu_hands = MenuHandSteadying::from_environment();
    std::atomic<bool> in_menu{false};
    std::atomic<bool> sharing{false};
    std::mutex view_lock;
    CameraView shared;  // under view_lock
    bool mirrored = false;
    std::mutex lock;
    SkeletonJoints joints{};
    uint64_t found = 0, taken = 0;
    bool observed = false, detected = false;
    std::chrono::steady_clock::time_point last_observation{}, last_pose{};
    std::atomic<bool> ever_found{false};
    std::jthread worker;

    void keep_view(const CameraFrame& frame, const PoseLandmarks* body) {
        if (!sharing.load(std::memory_order_relaxed)) return;
        std::lock_guard guard(view_lock);
        shared.picture = frame;
        shared.found = body != nullptr;
        if (body) shared.body = *body;
        shared.motion = estimator != nullptr;
        shared.mirrored = mirrored;
    }

    void run(std::stop_token stop) {
        CameraFrame frame;
        PoseLandmarks landmarks{};
        SkeletonJoints mapped{};
        uint64_t estimates = 0, held = 0;
        auto reported = std::chrono::steady_clock::now();
        auto last_picture = reported;
        double spent = 0;
        while (!stop.stop_requested()) {
            if (!camera->next(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!estimator) {  // a picture for the title, no body
                keep_view(frame, nullptr);
                continue;
            }
            const auto started = std::chrono::steady_clock::now();
            const double interval = std::chrono::duration<double>(started - last_picture).count();
            last_picture = started;
            bool body = estimator->estimate(frame, landmarks);
            if (body) {
                // Limb points the body cannot have reached are held first,
                // then everything is smoothed where the model read it, before
                // the picture becomes metres: the wandering is a picture's.
                stabilizer.stabilize(landmarks, interval);
                held += stabilizer.held();
                smoothing.smooth(landmarks, interval);
                body = pose_to_joints(landmarks, frame.width, frame.height, mapped, mirrored);
                if (body) menu_hands.steady(mapped, in_menu.load(std::memory_order_relaxed));
            } else {
                stabilizer.forget();
                smoothing.forget();
                menu_hands.forget();
            }
            keep_view(frame, body ? &landmarks : nullptr);
            const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            spent += took;
            note_pose_estimate(took);  // for the window's title
            ++estimates;
            {
                std::lock_guard guard(lock);
                observed = true;
                detected = body;
                last_observation = std::chrono::steady_clock::now();
                if (body) {
                    joints = mapped;
                    ++found;
                    last_pose = last_observation;
                    ever_found.store(true, std::memory_order_relaxed);
                }
            }
            // Once every five seconds: how well the camera is keeping up.
            const auto now = std::chrono::steady_clock::now();
            if (now - reported >= std::chrono::seconds(5)) {
                std::cerr << "NATIVE_CAMERA_PLAYER estimates=" << estimates << " tracked=" << found
                          << " average_ms=" << (estimates ? spent / double(estimates) : 0.0)
                          << " held_points=" << held << '\n';
                reported = now;
                estimates = 0;
                held = 0;
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

bool CameraPlayer::joints(SkeletonJoints& out, CameraTrackingStatus* status) {
    std::lock_guard guard(impl_->lock);
    if (status) {
        const auto now = std::chrono::steady_clock::now();
        status->detected = impl_->detected;
        status->observation_age_ms = impl_->observed ? std::chrono::duration<double,std::milli>(now-impl_->last_observation).count() : -1;
        status->pose_age_ms = impl_->found ? std::chrono::duration<double,std::milli>(now-impl_->last_pose).count() : -1;
    }
    if (impl_->found == impl_->taken) return false;
    out = impl_->joints;
    impl_->taken = impl_->found;
    return true;
}

void CameraPlayer::set_in_menu(bool in_menu) { impl_->in_menu.store(in_menu, std::memory_order_relaxed); }

void CameraPlayer::share_view(bool on) { impl_->sharing.store(on, std::memory_order_relaxed); }

bool CameraPlayer::view(CameraView& view) {
    std::lock_guard guard(impl_->view_lock);
    if (impl_->shared.picture.number == 0 || impl_->shared.picture.number == view.picture.number) return false;
    view = impl_->shared;
    return true;
}

bool CameraPlayer::control_range(CameraControl control, CameraControlRange& range) {
    return impl_->camera->control_range(control, range);
}
bool CameraPlayer::control(CameraControl control, CameraControlValue& value) {
    return impl_->camera->control(control, value);
}
bool CameraPlayer::set_control(CameraControl control, const CameraControlValue& value) {
    return impl_->camera->set_control(control, value);
}

bool CameraPlayer::tracking() const { return impl_->ever_found.load(std::memory_order_relaxed); }

}
