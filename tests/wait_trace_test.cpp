#include "wait_trace.h"
#include <sstream>
#include <stdexcept>
#include <iostream>

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        sfr::WaitTrace trace;
        sfr::WaitSite site{"event", 0x81230000, {42}, -1};
        uint64_t now = 0; unsigned clocks = 0, calls = 0;
        auto clock = [&] { ++clocks; return now; };
        auto runner = [&](auto operation) { now += 2; operation(); now += 7; };
        auto operation = [&] { ++calls; now += 11; };
        sfr::observe_wait(false, trace, site, runner, operation, clock);
        require(clocks == 0 && calls == 1 && trace.size() == 0, "disabled mode must only run callback");
        now = 0;
        sfr::observe_wait(true, trace, site, runner, operation, clock);
        const auto& row = trace.rows().front();
        require(row.count == 1 && row.native_ns == 11 && row.before_ns == 2 && row.resume_ns == 7,
                "native wait must be separated from admission and reacquisition");
        require(row.overshoot_ns == 0, "signalled wait has no timeout overshoot");
        uint32_t status = 258;
        auto timed = site; timed.timeout_ns = 5;
        now = 0;
        sfr::observe_wait(true, trace, timed, runner, operation, clock, &status);
        require(trace.rows().back().overshoot_ns == 6, "only actual timeout exceeds requested duration");
        status = 0; now = 0;
        sfr::observe_wait(true, trace, timed, runner, operation, clock, &status);
        require(trace.rows().back().overshoot_ns == 0, "successful finite event wait must not be labelled oversleep");
        auto multiple = site; multiple.kind = "wait_all"; multiple.targets = {42, 43};
        trace.record(multiple, 0, 1, 2, 3, 5, false);
        multiple.targets = {42, 44};
        trace.record(multiple, 0, 1, 2, 3, 5, false);
        require(trace.size() == 5, "different secondary targets must remain distinct");
        try {
            sfr::observe_wait(true, trace, site, runner, [] { throw std::runtime_error("original"); }, clock);
            require(false, "exception must propagate");
        } catch (const std::runtime_error& e) { require(std::string(e.what()) == "original", "preserve callback exception"); }
        require(trace.rows().back().failed == 1, "incomplete/error waits must be explicit");
        std::ostringstream out;
        trace.write_and_reset(out, 300, 5000);
        require(out.str().find("targets=0x2a,0x2c") != std::string::npos, "log all wait targets");
        require(out.str().find("native_ms=") != std::string::npos && trace.size() == 0, "flush consumes aggregates");
        for (unsigned i = 0; i < 1000; ++i) {
            site.caller = i;
            trace.record(site, 0, 1, 2, 3, 5, false);
        }
        require(trace.size() == sfr::WaitTrace::capacity && trace.dropped() == 1000 - sfr::WaitTrace::capacity,
                "diagnostic aggregation must have bounded storage and report overflow");
        trace.write_and_reset(out, 600, 5000);
        require(trace.dropped() == 0, "flush clears dropped count");
        site.kind = "frame_cap"; site.caller = 0x1234;
        for (unsigned i = 0; i < 300; ++i) {
            site.timeout_ns = 1000000 + i;
            trace.record(site, 0, 1, 1000000 + i, 3, 1000004 + i, false);
        }
        site.kind = "event"; site.timeout_ns = -1;
        trace.record(site, 0, 1, 2000000, 3, 2000004, false);
        require(trace.size() == 2 && trace.dropped() == 0 && trace.rows().front().count == 300,
                "varying pacing deadlines must not hide a newly encountered event wait");
        require(trace.rows().front().requested_min_ns == 1000000 && trace.rows().front().requested_max_ns == 1000299,
                "pacing aggregation must retain requested duration range");
        site.kind = "poll_50us"; site.timeout_ns = 50000;
        trace.record(site, 0, 1, 1000000, 3, 1000004, false);
        require(trace.rows().back().overshoot_ns == 950000, "short host polling sleeps must expose overshoot");
        sfr::WaitTrace worker;
        worker.record(site, 0, 1, 2000000, 3, 2000004, false);
        const auto main_rows = trace.size();
        std::ostringstream worker_output;
        worker.write_and_reset(worker_output, 100, 5000, 7);
        require(worker_output.str().find("WAIT_TRACE_WINDOW frame=100 guest_id=7 ") != std::string::npos &&
                worker_output.str().find("WAIT_TRACE frame=100 guest_id=7 ") != std::string::npos,
                "both worker window and detail must identify their guest");
        require(worker.size() == 0 && trace.size() == main_rows,
                "flushing a worker must not reset main-thread samples");
        std::ostringstream main_output;
        trace.write_and_reset(main_output, 100, 5000);
        require(main_output.str().find("guest_id=1 ") != std::string::npos,
                "existing callers must identify the main guest by default");
        std::cout << "wait trace tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
