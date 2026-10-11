#include "title_stats.h"

#include <atomic>
#include <chrono>

namespace sfr {
namespace {
using clock = std::chrono::steady_clock;
std::atomic<int64_t> last_frame{0};        // clock ticks of the last present
std::atomic<int64_t> worst_frame_us{0};
std::atomic<uint32_t> kinect_frames{0}, pose_estimates{0};
std::atomic<int64_t> pose_us{0};
std::atomic<void*> window{nullptr};
}

void set_game_window(void* made) { window.store(made, std::memory_order_relaxed); }
void* game_window() { return window.load(std::memory_order_relaxed); }

void note_frame_presented() {
    const int64_t now = clock::now().time_since_epoch().count();
    const int64_t before = last_frame.exchange(now, std::memory_order_relaxed);
    if (!before) return;
    const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(clock::duration(now - before)).count();
    int64_t worst = worst_frame_us.load(std::memory_order_relaxed);
    while (us > worst && !worst_frame_us.compare_exchange_weak(worst, us, std::memory_order_relaxed)) {}
}

void note_kinect_frame() { kinect_frames.fetch_add(1, std::memory_order_relaxed); }

void note_pose_estimate(double milliseconds) {
    pose_us.fetch_add(int64_t(milliseconds * 1000), std::memory_order_relaxed);
    pose_estimates.fetch_add(1, std::memory_order_relaxed);
}

TitleStats take_title_stats() {
    TitleStats stats;
    stats.worst_frame_ms = worst_frame_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    stats.kinect_frames = kinect_frames.exchange(0, std::memory_order_relaxed);
    stats.pose_estimates = pose_estimates.exchange(0, std::memory_order_relaxed);
    stats.pose_ms = pose_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    return stats;
}
}
