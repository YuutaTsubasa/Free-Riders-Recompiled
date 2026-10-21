#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace sfr {

// SFR_NUI_ANALYSIS_TRACE=1: observation lines for the study of how the title
// uses its skeletons (docs/kinect-sensor.md, "觀察用 trace"). They change
// nothing the title does; each kind writes lines starting NUI_TRACE_<KIND>,
// spaced out and capped (NuiTraceLimiter), so game.log stays small however
// long a session runs.
inline bool nui_analysis_trace() {
    static const bool enabled = [] {
        const char* text = std::getenv("SFR_NUI_ANALYSIS_TRACE");
        return text && *text == '1';
    }();
    return enabled;
}

// One kind's budget: at most per_second lines a second, spaced evenly (a
// call asking for n lines is answered at most every n / per_second
// seconds), and at most total lines in all, after which one line says the
// kind has stopped. Thread safe.
class NuiTraceLimiter {
public:
    NuiTraceLimiter(const char* kind, double per_second, uint64_t total)
        : kind_(kind), interval_(per_second > 0 ? 1.0 / per_second : 1.0), total_(total) {}
    bool take(uint32_t lines = 1) {
        if (!nui_analysis_trace()) return false;
        std::lock_guard guard(lock_);
        if (stopped_) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_) return false;
        if (written_ + lines > total_) {
            stopped_ = true;
            std::cerr << "NUI_TRACE_" << kind_ << " stopped after " << written_ << " lines (cap)\n";
            return false;
        }
        written_ += lines;
        next_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(interval_ * lines));
        return true;
    }
private:
    const char* kind_;
    double interval_;
    uint64_t total_, written_ = 0;
    bool stopped_ = false;
    std::chrono::steady_clock::time_point next_{};
    std::mutex lock_;
};

}
