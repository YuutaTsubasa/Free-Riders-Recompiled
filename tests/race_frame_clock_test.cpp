#include "race_frame_clock.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message) {
    require(std::abs(actual-expected)<=tolerance,message);
}
void transitions() {
    sfr::RaceFrameClock clock;
    require(!clock.update(100,0),"outside a race retains original timing");
    const auto first=clock.update(1'000'000'000,7);
    require(first.has_value(),"entering a race returns a time step");
    near(first->frames,1,1e-6,"first frame does not include loading time");
    near(clock.update(1'050'000'000,7)->frames,3,1e-6,"20 FPS advances three original frames");
    near(clock.update(2'000'000'000,8)->frames,1,1e-6,"another race resets its time origin");
    require(!clock.update(3'000'000'000,0),"leaving a race disables the override");
    near(clock.update(4'000'000'000,8)->frames,1,1e-6,"reentering the same address does not replay menu time");
    near(clock.update(100,8)->frames,1,1e-6,"backwards clock resets without unsigned underflow");
}
void elapsed_time_is_preserved() {
    for (const uint64_t interval : {8'333'333ull,16'666'667ull,33'333'333ull,50'000'000ull}) {
        sfr::RaceFrameClock clock;
        uint64_t now=10'000'000'000;
        clock.update(now,1);
        double simulation=0;
        for (int i=0;i<10'000;++i) {
            now+=interval;
            const auto step=clock.update(now,1);
            require(step && !step->clamped,"ordinary frame cadence is not clamped");
            simulation+=step->frames/60.0;
        }
        near(simulation,double(interval)*10'000e-9,0.001,"120/60/30/20 FPS preserves elapsed simulation time");
    }
    constexpr std::array<uint64_t,7> intervals{17'001'234,39'172'658,55'192'374,90'128'377,12'018'244,41'975'233,48'237'384};
    sfr::RaceFrameClock mixed;
    uint64_t now=0; double simulation=0;
    mixed.update(now,5);
    for(int i=0;i<3000;++i) {
        now+=intervals[i%intervals.size()];
        simulation+=mixed.update(now,5)->frames/60.0;
    }
    near(simulation,double(now)*1e-9,0.001,"changing frame rate does not quantize or accumulate drift");
}
void discontinuities_are_bounded_and_observable() {
    sfr::RaceFrameClock clock;
    clock.update(0,1);
    const auto stalled=clock.update(2'000'000'000,1);
    require(stalled && stalled->clamped,"long stall is reported, not silently treated as real-time");
    near(stalled->elapsed_seconds,2,1e-9,"raw elapsed time remains available for diagnostics");
    near(stalled->frames,15,1e-6,"a single physics update is bounded to 250 ms");
    near(clock.update(2'050'000'000,1)->frames,3,1e-6,"stall is not replayed on later frames");
    const auto repeated=clock.update(2'050'000'000,1);
    require(repeated && repeated->clamped && repeated->frames>0,"equal timestamps give a small positive forced step");
    near(repeated->elapsed_seconds,0,0,"equal timestamps do not invent raw elapsed time");
    clock.update(std::numeric_limits<uint64_t>::max()-100'000'000,2);
    near(clock.update(std::numeric_limits<uint64_t>::max()-50'000'000,2)->frames,3,1e-6,"large monotonic epoch does not lose frame precision");
}
}
int main() {
    try {
        transitions(); elapsed_time_is_preserved(); discontinuities_are_bounded_and_observable();
        std::cout << "Race frame clock checks passed\n";
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
