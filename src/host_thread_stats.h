#pragma once
// Where a frame's time goes between the title's main thread and the render
// thread, in a few numbers a player's game.log can carry (Issue #176). A
// Pixel 10a (Tensor G4) drew a race frame in ~35 ms of main-thread time and
// ran an undrawn one in ~10, where a Huawei P30 Pro (Kirin 980, slower
// cores) took ~24 and ~14: the extra time is in drawing, yet the GPU and
// the guest's own waits account for none of it. What the log could not say:
// how long the main thread waits for the render thread to empty its queue
// (every clear, present and resolve does), how long the render thread works,
// which cores each ran on, and the clocks those cores were given.
//
// HOST_THREADS (every 120 frames, Android and Windows), averages per frame:
//   drawn            frames drawn of the 120,
//   drawn_ms         main thread's work in a drawn frame (the frame less its
//                    pacing wait), undrawn_ms the same for an undrawn one,
//   draw_ms          main thread's drawing in a drawn frame (native_draw),
//                    record_ms the renderer's part of it (writing the upload
//                    ring, queueing the draw),
//   cpu_ms           main thread's CPU time (less than the work: it waited
//                    or was not given a core),
//   render_wait_ms   main thread waiting for the render thread's queue to
//                    empty, render_waits how many times a frame,
//   render_busy_ms   render thread recording (bursts from finding work to
//                    an empty queue),
//   main_on/render_on that time by core speed (cpu_capacity, as CORE_PLAN
//                    prints it): speed:ms,...
//   main_mask        the cores the main thread may run on now (0 unknown),
//   mhz              each speed's current/maximum clock, 0 if unreadable.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#if defined(__linux__)
#include <sched.h>
#include <time.h>
#elif defined(_WIN32)
#include "host_timing.h"
#endif

namespace sfr {

struct RenderThreadStats {
    std::atomic<uint64_t> wait_ns{0}, waits{0};
    std::atomic<uint64_t> busy_ns_by_cpu[64] = {};
};
inline RenderThreadStats render_thread_stats;

inline uint64_t host_monotonic_ns() {
#if defined(__linux__)
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1'000'000'000u + uint64_t(now.tv_nsec);
#elif defined(_WIN32)
    return windows_monotonic_ns();
#else
    return 0;
#endif
}

// The core the calling thread runs on now, or -1.
inline int host_current_cpu() {
#if defined(__linux__)
    const int cpu = sched_getcpu();
    return cpu >= 0 && cpu < 64 ? cpu : -1;
#elif defined(_WIN32)
    return windows_current_cpu();
#else
    return -1;
#endif
}

inline uint64_t host_thread_cpu_ns() {
#if defined(__linux__)
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return uint64_t(now.tv_sec) * 1'000'000'000u + uint64_t(now.tv_nsec);
#elif defined(_WIN32)
    return windows_thread_cpu_ns();
#else
    return 0;
#endif
}

// Per-core times grouped by core speed, in order of the speeds' first cores:
// "191:0.0,741:2.5,1024:30.1" (ms per frame). Cores of unknown speed (0)
// are grouped as speed 0.
inline std::string time_by_speed(const double (&ms)[64], const long (&speed)[64], double frames) {
    std::string text;
    bool done[64] = {};
    for (int first = 0; first < 64; ++first) {
        if (done[first]) continue;
        double sum = 0;
        bool any = false;
        for (int cpu = first; cpu < 64; ++cpu)
            if (!done[cpu] && speed[cpu] == speed[first]) {
                done[cpu] = true;
                sum += ms[cpu];
                any = any || speed[cpu] > 0 || ms[cpu] > 0;
            }
        if (!any) continue;
        char part[48];
        std::snprintf(part, sizeof part, "%s%ld:%.1f", text.empty() ? "" : ",", speed[first],
                      frames > 0 ? sum / frames : 0.0);
        text += part;
    }
    return text.empty() ? "none" : text;
}

// The render thread's side: one burst of recording, from finding work to
// finding the queue empty. The core is asked only every eighth burst (a
// race frame has hundreds of them), and the bursts since are counted there.
class RenderBurstClock {
public:
    void begin() { start_ns_ = host_monotonic_ns(); }
    void end() {
        if (!start_ns_) return;
        pending_ns_ += host_monotonic_ns() - start_ns_;
        start_ns_ = 0;
        if (++bursts_ % 8 && pending_ns_ < 1'000'000) return;
        const int cpu = host_current_cpu();
        if (cpu >= 0) render_thread_stats.busy_ns_by_cpu[cpu].fetch_add(pending_ns_, std::memory_order_relaxed);
        pending_ns_ = 0;
    }
    bool running() const { return start_ns_ != 0; }
private:
    uint64_t start_ns_ = 0, pending_ns_ = 0, bursts_ = 0;
};

// The main thread's side, once a frame.
class HostThreadWindow {
public:
    // work_ms: the frame less its pacing wait; draw_ms and record_ms the
    // frame's drawing on the main thread. Returns the log line every 120
    // frames, else an empty string.
    std::string frame(uint64_t present, double work_ms, bool drawn, double draw_ms, double record_ms,
                      const long (&speed)[64]) {
        const uint64_t cpu_ns = host_thread_cpu_ns();
        if (last_cpu_ns_) cpu_ms_ += double(cpu_ns - last_cpu_ns_) / 1e6;
        last_cpu_ns_ = cpu_ns;
        const int cpu = host_current_cpu();
        if (cpu >= 0 && work_ms > 0) main_ms_[cpu] += work_ms;
        (drawn ? drawn_ms_ : undrawn_ms_) += work_ms > 0 ? work_ms : 0;
        drawn_ += drawn ? 1 : 0;
        draw_ms_ += draw_ms;
        record_ms_ += record_ms;
        if (++frames_ < 120) return {};
        double render_ms[64] = {};
        double busy = 0;
        for (int core = 0; core < 64; ++core) {
            render_ms[core] = double(render_thread_stats.busy_ns_by_cpu[core].exchange(0, std::memory_order_relaxed)) / 1e6;
            busy += render_ms[core];
        }
        const double wait = double(render_thread_stats.wait_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
        const double waits = double(render_thread_stats.waits.exchange(0, std::memory_order_relaxed));
        const double n = frames_;
        const unsigned undrawn = frames_ - drawn_;
        char head[400];
        std::snprintf(head, sizeof head,
                      "HOST_THREADS frame=%llu drawn=%u drawn_ms=%.2f undrawn_ms=%.2f draw_ms=%.2f record_ms=%.2f cpu_ms=%.2f render_wait_ms=%.2f "
                      "render_waits=%.1f render_busy_ms=%.2f main_on=",
                      static_cast<unsigned long long>(present), drawn_, drawn_ ? drawn_ms_ / drawn_ : 0.0,
                      undrawn ? undrawn_ms_ / undrawn : 0.0, drawn_ ? draw_ms_ / drawn_ : 0.0,
                      drawn_ ? record_ms_ / drawn_ : 0.0, cpu_ms_ / n, wait / n, waits / n, busy / n);
        std::string line = head + time_by_speed(main_ms_, speed, n) + " render_on=" + time_by_speed(render_ms, speed, n) +
                           " main_mask=" + allowed_cores() + " mhz=" + clocks(speed) + '\n';
        *this = HostThreadWindow{};
        last_cpu_ns_ = cpu_ns;
        return line;
    }
private:
    // The cores the calling (main) thread may run on now, as a hex mask: the
    // core plan sets it at the start, and the title's own KeSetAffinityThread
    // calls can replace it later.
    static std::string allowed_cores() {
        uint64_t mask = 0;
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof set, &set) == 0)
            for (int cpu = 0; cpu < 64; ++cpu)
                if (CPU_ISSET(cpu, &set)) mask |= uint64_t{1} << cpu;
#endif
        char text[24];
        std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(mask));
        return text;
    }
    // Current/maximum clock of the first core of each speed, in MHz.
    static std::string clocks(const long (&speed)[64]) {
        std::string text;
        for (int cpu = 0; cpu < 64; ++cpu) {
            if (speed[cpu] <= 0) continue;
            bool seen = false;
            for (int before = 0; before < cpu; ++before) seen = seen || speed[before] == speed[cpu];
            if (seen) continue;
            char part[64];
            std::snprintf(part, sizeof part, "%s%ld:%ld/%ld", text.empty() ? "" : ",", speed[cpu],
                          read_khz(cpu, "scaling_cur_freq") / 1000, read_khz(cpu, "scaling_max_freq") / 1000);
            text += part;
        }
        return text.empty() ? "none" : text;
    }
    static long read_khz(int cpu, const char* name) {
#if defined(__linux__)
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, name);
        long value = 0;
        if (FILE* file = std::fopen(path, "r")) {
            if (std::fscanf(file, "%ld", &value) != 1) value = 0;
            std::fclose(file);
        }
        return value;
#else
        (void)cpu, (void)name;
        return 0;
#endif
    }
    unsigned frames_ = 0, drawn_ = 0;
    double drawn_ms_ = 0, undrawn_ms_ = 0, cpu_ms_ = 0, draw_ms_ = 0, record_ms_ = 0;
    double main_ms_[64] = {};
    uint64_t last_cpu_ns_ = 0;
};

}
