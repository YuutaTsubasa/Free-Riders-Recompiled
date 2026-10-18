#pragma once
// Android's performance hints (ADPF, API 33+): the title's main thread (and
// the render thread, if it has started by the first present; on the Thor it
// had not) form a hint session whose target is a sixtieth of a
// second, and every frame reports how long the main thread worked (the frame
// less the pacing wait). Where the phone's power HAL supports it, it raises
// clocks or moves the threads to faster cores when the work overruns the
// target, instead of waiting for the scheduler's load tracking.
// SFR_PERF_HINT=1|0 (default 0: experiment). Loaded with dlsym, as the app
// still starts on API 29. Elsewhere the calls do nothing.
#include <cstdint>
#include <cstdlib>
#if defined(__ANDROID__)
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace sfr {

#if defined(__ANDROID__)
namespace performance_hint_detail {
inline std::atomic<int32_t> render_tid{0};
inline bool enabled() {
    static const bool on = [] {
        const char* text = std::getenv("SFR_PERF_HINT");
        return text && *text == '1';
    }();
    return on;
}
}

// The render thread names itself before the first frame is reported.
inline void note_render_thread_for_hints() {
    performance_hint_detail::render_tid.store(int32_t(gettid()), std::memory_order_relaxed);
}

// Called on the main thread once a frame with the frame's work time.
inline void report_frame_work(double work_ms) {
    using namespace performance_hint_detail;
    if (!enabled()) return;
    struct Session;
    using GetManager = void* (*)();
    using Create = Session* (*)(void*, const int32_t*, size_t, int64_t);
    using Report = int (*)(Session*, int64_t);
    static Report report = nullptr;
    static Session* session = [] () -> Session* {
        void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        auto get = library ? reinterpret_cast<GetManager>(dlsym(library, "APerformanceHint_getManager")) : nullptr;
        auto create = library ? reinterpret_cast<Create>(dlsym(library, "APerformanceHint_createSession")) : nullptr;
        report = library ? reinterpret_cast<Report>(dlsym(library, "APerformanceHint_reportActualWorkDuration")) : nullptr;
        void* manager = get ? get() : nullptr;
        int32_t tids[2] = {int32_t(gettid()), render_tid.load(std::memory_order_relaxed)};
        const size_t count = tids[1] ? 2 : 1;
        Session* made = manager && create && report ? create(manager, tids, count, 16'666'667) : nullptr;
        std::fprintf(stderr, "PERF_HINT session=%d threads=%zu main=%d render=%d\n", made ? 1 : 0, count, tids[0], tids[1]);
        return made;
    }();
    if (!session || work_ms <= 0) return;
    report(session, int64_t(work_ms * 1e6));
}
#else
inline void note_render_thread_for_hints() {}
inline void report_frame_work(double) {}
#endif

}
