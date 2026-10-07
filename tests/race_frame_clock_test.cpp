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
void ui_timelines_keep_time_and_events() {
    // The guest evaluates actions for the current frame, then advances at
    // most one frame if floor(position) changes. Scaling position alone
    // loses both time and the intervening frame actions.
    for (const int fps : {20,24,30,60,120}) {
        for (const float speed : {0.5f,1.0f}) {
            double position=0; int frame=0, last_position=0;
            int last_event=-1; unsigned events=0;
            for (int i=0;i<fps*10;++i) {
                sfr::for_each_race_ui_step(60.0f/fps,[&](float step) {
                    require(step>0 && step<=1,"UI substeps cannot skip an original frame");
                    if (frame!=last_event) {
                        require(frame==last_event+1,"every intermediate timeline event is evaluated");
                        last_event=frame; ++events;
                    }
                    position+=double(speed)*step;
                    const int next=int(std::floor(position));
                    if(next!=last_position) ++frame;
                    last_position=next;
                    return true;
                });
            }
            near(frame,600*speed,1,"UI playback duration is independent of rendering FPS");
            require(events>=unsigned(frame),"events are not lost while catching up");
        }
    }
    unsigned calls=0;
    sfr::for_each_race_ui_step(15,[&](float) {return ++calls<3;});
    require(calls==3,"completion is returned before processing another animation cycle");
    calls=0;
    sfr::for_each_race_ui_step(1000,[&](float) {++calls;return true;});
    require(calls==15,"long stalls cannot cause unbounded UI work");
    for(float invalid : {0.0f,-1.0f,std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN()}) {
        calls=0;
        sfr::for_each_race_ui_step(invalid,[&](float step) {
            ++calls; near(step,1,0,"invalid UI deltas retain one original update");return true;
        });
        require(calls==1,"invalid UI deltas cannot spin or freeze the timeline");
    }
}
}
// At about 60 fps a frame's step is exactly one original frame: a little
// jitter (15.2 to 18.2 ms) never reaches the physics, and the time it owes is
// carried, not lost. Rates far from 60 keep their fractional steps.
void sixty_fps_steps_whole_frames() {
    constexpr std::array<uint64_t,6> jitter{15'200'000,18'200'000,16'000'000,17'400'000,16'900'000,16'300'000};
    sfr::RaceFrameClock clock;
    uint64_t now=0; double simulation=0; unsigned whole=0;
    clock.update(now,3);
    for (int i=0;i<6000;++i) {
        now+=jitter[i%jitter.size()];
        const float frames=clock.update(now,3)->frames;
        whole+=frames==1.0f;
        simulation+=frames/60.0;
    }
    require(whole==6000,"every step near 60 fps is exactly one frame");
    near(simulation,double(now)*1e-9,0.002,"whole frames keep the race on real time");
    sfr::RaceFrameClock fast;
    fast.update(0,4);
    near(fast.update(8'333'333,4)->frames,0.5,1e-4,"120 fps keeps half steps");
    sfr::RaceFrameClock slow;
    slow.update(0,5);
    near(slow.update(25'000'000,5)->frames,1.5,1e-4,"40 fps keeps its longer steps");
}
void race_present_limit_counts_only_the_race() {
    sfr::RacePresentLimit off(0);
    for (int i=0;i<1000;++i) require(!off.reached(true),"no limit never ends the run");
    sfr::RacePresentLimit limit(3);
    for (int i=0;i<500;++i) require(!limit.reached(false),"menus and loading are not counted");
    require(!limit.reached(true) && !limit.reached(true),"the first race presents count");
    require(!limit.reached(false),"a present without the race flag is not counted");
    require(limit.reached(true),"the third race present ends the run");
    require(limit.counted()==3,"three race presents were counted");
}

int main() {
    try {
        transitions(); elapsed_time_is_preserved(); discontinuities_are_bounded_and_observable();
        sixty_fps_steps_whole_frames();
        ui_timelines_keep_time_and_events();
        race_present_limit_counts_only_the_race();
        std::cout << "Race frame clock checks passed\n";
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
