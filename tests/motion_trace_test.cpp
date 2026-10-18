#include "motion_trace.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void rate_limits_a_flood() {
    using namespace std::chrono_literals;
    sfr::TraceRate rate(20);
    const auto start = sfr::TraceRate::Clock::time_point{} + 100s;
    int written = 0;
    // A detector flipping every frame for a second at 60 frames a second.
    for (int frame = 0; frame < 60; ++frame) written += rate.allow(start + frame * 16ms);
    require(written == 20, "at most 20 lines in a second");
    require(rate.allow(start + 1001ms), "the next second writes again");
    require(rate.dropped() == 40, "the next line says how many were held back");
    require(rate.allow(start + 1100ms) && rate.dropped() == 0, "nothing held back since then");
}

void cost_reports_mean_and_longest() {
    using namespace std::chrono_literals;
    sfr::TraceCost cost;
    require(cost.calls() == 0 && cost.mean_us() == 0 && cost.max_us() == 0, "nothing measured yet");
    cost.add(10us);
    cost.add(30us);
    require(cost.calls() == 2 && cost.mean_us() == 20 && cost.max_us() == 30, "mean and longest in microseconds");
    cost.clear();
    require(cost.calls() == 0, "a report starts the count again");
}
}

int main() {
    try {
        rate_limits_a_flood();
        cost_reports_mean_and_longest();
        std::cout << "Motion trace checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
