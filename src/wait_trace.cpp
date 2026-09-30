#include "wait_trace.h"
#include <algorithm>
#include <iomanip>

namespace sfr {
void WaitTrace::record(const WaitSite& site, uint32_t status, uint64_t before, uint64_t native,
                       uint64_t resume, uint64_t total, bool failed) noexcept {
    auto key = site;
    // Frame cap sleeps toward a moving deadline, not a fixed timeout. Keep
    // their requested range without making each frame a new aggregation key.
    if (key.kind == "frame_cap") key.timeout_ns = -1;
    size_t i = 0;
    for (; i < size_; ++i)
        if (rows_[i].site == key && rows_[i].status == status && bool(rows_[i].failed) == failed) break;
    if (i == size_) {
        if (size_ == capacity) {
            ++dropped_; dropped_total_ns_ += total; dropped_native_ns_ += native; dropped_resume_ns_ += resume;
            return;
        }
        rows_[size_++] = {key, status};
    }
    auto& row = rows_[i];
    if (!row.count) row.requested_min_ns = row.requested_max_ns = site.timeout_ns;
    else {
        row.requested_min_ns = std::min(row.requested_min_ns, site.timeout_ns);
        row.requested_max_ns = std::max(row.requested_max_ns, site.timeout_ns);
    }
    ++row.count; row.failed += failed;
    row.before_ns += before; row.native_ns += native; row.resume_ns += resume;
    row.max_native_ns = std::max(row.max_native_ns, native);
    const bool sleep = site.kind == "delay" || site.kind == "poll_50us" || site.kind == "poll_100us";
    if (!failed && site.timeout_ns >= 0 && (status == 258 || sleep) && native > uint64_t(site.timeout_ns))
        row.overshoot_ns += native - uint64_t(site.timeout_ns);
}
void WaitTrace::write_and_reset(std::ostream& out, uint32_t frame, double window_ms, uint32_t guest_id) {
    const auto flags = out.flags(); const auto precision = out.precision();
    out << std::dec << std::fixed << std::setprecision(4);
    out << "WAIT_TRACE_WINDOW frame=" << frame << " guest_id=" << guest_id << " window_ms=" << window_ms
        << " sites=" << size_ << " dropped=" << dropped_ << " dropped_total_ms=" << dropped_total_ns_ / 1e6
        << " dropped_native_ms=" << dropped_native_ns_ / 1e6 << " dropped_resume_ms=" << dropped_resume_ns_ / 1e6 << '\n';
    for (const auto& row : rows()) {
        out << "WAIT_TRACE frame=" << frame << " guest_id=" << guest_id << " kind=" << row.site.kind << " caller=0x" << std::hex << row.site.caller
            << " targets=";
        for (uint32_t i = 0; i < row.site.targets.count; ++i) {
            if (i) out << ',';
            out << "0x" << row.site.targets.values[i];
        }
        if (!row.site.targets.count) out << "none";
        out << " owner_object=0x" << row.site.owner << " status=0x" << row.status << std::dec
            << " timeout_ns=" << row.site.timeout_ns << " requested_min_ns=" << row.requested_min_ns
            << " requested_max_ns=" << row.requested_max_ns << " count=" << row.count << " failed=" << row.failed
            << " before_ms=" << row.before_ns / 1e6 << " native_ms=" << row.native_ns / 1e6
            << " resume_ms=" << row.resume_ns / 1e6 << " overshoot_ms=" << row.overshoot_ns / 1e6
            << " max_native_ms=" << row.max_native_ns / 1e6 << '\n';
    }
    out.flags(flags); out.precision(precision);
    size_ = 0; dropped_ = dropped_total_ns_ = dropped_native_ns_ = dropped_resume_ns_ = 0;
}
}
