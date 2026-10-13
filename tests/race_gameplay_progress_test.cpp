#include "race_gameplay_progress.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t root = 0x12340000;
constexpr uint32_t owner = 0x56780000;
constexpr double step_ms = static_cast<double>(16.66666794f);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

sfr::GameplayObservation observation(uint32_t phase, double elapsed, double countdown,
                                     bool paused = false) {
    sfr::GameplayObservation value;
    value.supported = true;
    value.object = root;
    value.controller = owner;
    value.phase = phase;
    value.elapsed_ms = elapsed;
    value.countdown_ms = countdown;
    value.paused = paused;
    value.config = {3, 1, 7, 0, 2, 4, 9};
    return value;
}

void prestart(sfr::RaceGameplayProgress& progress) {
    progress.constructed(root);
    progress.observe(observation(9, 0, 5000));
}

// Observe all 300 countdown updates. The 300th update is the phase 11,
// elapsed-zero boundary; phase 11's next completed update is race tick one.
void finish_countdown(sfr::RaceGameplayProgress& progress) {
    for (unsigned n = 1; n < 300; ++n)
        progress.observe(observation(10, 0, 5000.0 - step_ms * n));
    progress.observe(observation(11, 0, 0));
}

void epoch_and_countdown_boundary() {
    sfr::RaceGameplayProgress unseen;
    unseen.observe(observation(9, 0, 5000));
    require(unseen.snapshot(root).run == 0 && unseen.snapshot(root).tick == 0,
            "pre-construction observations cannot invent an epoch");

    sfr::RaceGameplayProgress progress;
    prestart(progress);
    const auto run = progress.snapshot(root).run;
    require(run != 0 && progress.snapshot(root).state == sfr::GameplayState::prestart,
            "construction plus a known prestart phase establishes the run");

    progress.observe(observation(10, 0, 5000.0 - step_ms));
    auto sample = progress.snapshot(root);
    require(sample.state == sfr::GameplayState::advancing &&
            sample.stage == sfr::GameplayStage::countdown && sample.tick == 1 &&
            sample.countdown_tick == 1 && sample.race_tick == 0 && sample.start_observed,
            "first phase 10 sample records countdown update one");

    for (unsigned n = 2; n < 300; ++n) {
        progress.observe(observation(10, 0, 5000.0 - step_ms * n));
        sample = progress.snapshot(root);
        require(sample.tick == n && sample.countdown_tick == n && sample.race_tick == 0,
                "each phase 10 countdown update is retained exactly once");
    }
    progress.observe(observation(11, 0, 0));
    sample = progress.snapshot(root);
    require(sample.state == sfr::GameplayState::advancing && sample.stage == sfr::GameplayStage::countdown &&
            sample.tick == 300 && sample.countdown_tick == 300 && sample.race_tick == 0 &&
            sample.elapsed_ms == 0 && sample.countdown_ms == 0,
            "phase 11 at elapsed zero retains the final countdown update without a race tick");

    progress.observe(observation(11, step_ms, 0));
    sample = progress.snapshot(root);
    require(sample.stage == sfr::GameplayStage::racing && sample.tick == 301 &&
            sample.countdown_tick == 300 && sample.race_tick == 1,
            "the next phase 11 update becomes race tick one without a gap or double count");

    progress.constructed(root);
    sample = progress.snapshot(root);
    require(sample.run > run && sample.tick == 0 && sample.countdown_tick == 0 &&
            sample.race_tick == 0 && sample.state == sfr::GameplayState::unseen,
            "constructor reuse of the same address begins a new epoch");
}

void combined_long_run_and_pause_gates() {
    sfr::RaceGameplayProgress progress;
    prestart(progress);
    finish_countdown(progress);
    double elapsed = 0;
    for (unsigned race = 1; race <= 5100; ++race) {
        elapsed += step_ms;
        progress.observe(observation(11, elapsed, 0));
        const auto sample = progress.snapshot(root);
        require(sample.state == sfr::GameplayState::advancing && sample.stage == sfr::GameplayStage::racing &&
                sample.tick == 300 + race && sample.countdown_tick == 300 && sample.race_tick == race,
                "5400 combined updates contain 300 countdown and 5100 race updates");
    }
    require(progress.snapshot(root).tick == 5400, "combined update count reaches 5400");

    sfr::RaceGameplayProgress paused;
    prestart(paused);
    paused.observe(observation(10, 0, 5000.0 - step_ms));
    paused.observe(observation(10, 0, 5000.0 - step_ms, true));
    auto sample = paused.snapshot(root);
    require(sample.state == sfr::GameplayState::paused && sample.stage == sfr::GameplayStage::countdown &&
            sample.tick == 1 && sample.countdown_tick == 1,
            "pause during countdown does not advance either counter");
    paused.observe(observation(10, 0, 5000.0 - 2 * step_ms));
    require(paused.snapshot(root).tick == 2 && paused.snapshot(root).countdown_tick == 2,
            "one resumed countdown update adds one combined tick");
    for (unsigned n = 3; n < 300; ++n)
        paused.observe(observation(10, 0, 5000.0 - step_ms * n));
    paused.observe(observation(11, 0, 0));
    paused.observe(observation(11, 0, 0, true));
    sample = paused.snapshot(root);
    require(sample.state == sfr::GameplayState::paused && sample.stage == sfr::GameplayStage::racing &&
            sample.tick == 300 && sample.countdown_tick == 300 && sample.race_tick == 0,
            "pause at racing entry does not add a combined or race tick");
    paused.observe(observation(11, step_ms, 0));
    sample = paused.snapshot(root);
    require(sample.state == sfr::GameplayState::advancing && sample.tick == 301 && sample.race_tick == 1,
            "one resumed race update adds exactly one combined tick");
}

void late_start_unknown_phases_and_invalid_updates_fail_closed() {
    sfr::RaceGameplayProgress late;
    late.constructed(root);
    late.observe(observation(10, 0, 5000.0 - 2 * step_ms));
    require(late.snapshot(root).state == sfr::GameplayState::invalid && late.snapshot(root).tick == 0,
            "late attachment after two countdown updates cannot invent a start");

    sfr::RaceGameplayProgress unknown;
    unknown.constructed(root);
    unknown.observe(observation(17, 0, 5000));
    require(unknown.snapshot(root).state == sfr::GameplayState::invalid,
            "unknown phase 17 cannot become a prestart witness");
    sfr::RaceGameplayProgress terminal;
    terminal.constructed(root);
    terminal.observe(observation(12, 0, 5000));
    require(terminal.snapshot(root).state == sfr::GameplayState::outside,
            "known terminal phase cannot become a prestart witness");

    const double bad_elapsed[] = {step_ms, 3 * step_ms, 1.5 * step_ms, 0,
                                  -1, std::numeric_limits<double>::quiet_NaN()};
    const char* reasons[] = {"zero delta", "two-step delta", "fractional delta", "backwards timer",
                             "negative timer", "NaN timer"};
    for (unsigned i = 0; i < sizeof(bad_elapsed) / sizeof(bad_elapsed[0]); ++i) {
        sfr::RaceGameplayProgress progress;
        prestart(progress);
        finish_countdown(progress);
        progress.observe(observation(11, step_ms, 0));
        progress.observe(observation(11, bad_elapsed[i], 0));
        const auto invalid = progress.snapshot(root);
        require(invalid.state == sfr::GameplayState::invalid && invalid.tick == 301, reasons[i]);
        progress.observe(observation(11, 2 * step_ms, 0));
        require(progress.snapshot(root).state == sfr::GameplayState::invalid &&
                progress.snapshot(root).tick == 301,
                "invalid state is sticky and cannot add more ticks");
    }

    sfr::RaceGameplayProgress bad_countdown;
    prestart(bad_countdown);
    bad_countdown.observe(observation(10, 0, 5000.0 - 2 * step_ms));
    require(bad_countdown.snapshot(root).state == sfr::GameplayState::invalid,
            "a two-update countdown jump is rejected");
}

void identity_exit_limits_and_output() {
    auto prepare_countdown = [] {
        sfr::RaceGameplayProgress progress;
        prestart(progress);
        progress.observe(observation(10, 0, 5000.0 - step_ms));
        return progress;
    };
    auto unsupported = prepare_countdown();
    auto wrong_contract = observation(10, 0, 5000.0 - 2 * step_ms);
    wrong_contract.supported = false;
    unsupported.observe(wrong_contract);
    require(unsupported.snapshot(root).state == sfr::GameplayState::invalid,
            "losing the supported contract invalidates the run");

    auto owner_change = prepare_countdown();
    auto changed_owner = observation(10, 0, 5000.0 - 2 * step_ms);
    changed_owner.controller++;
    owner_change.observe(changed_owner);
    require(owner_change.snapshot(root).state == sfr::GameplayState::invalid,
            "controller identity change invalidates the run");

    auto config_change = prepare_countdown();
    auto changed_config = observation(10, 0, 5000.0 - 2 * step_ms);
    changed_config.config[2]++;
    config_change.observe(changed_config);
    require(config_change.snapshot(root).state == sfr::GameplayState::invalid,
            "configuration change invalidates the run");

    auto root_change = prepare_countdown();
    auto changed_root = observation(10, 0, 5000.0 - 2 * step_ms);
    changed_root.object++;
    root_change.observe(changed_root);
    require(root_change.snapshot(root).state == sfr::GameplayState::invalid,
            "root identity change invalidates the run");

    sfr::RaceGameplayProgress reentry;
    prestart(reentry);
    finish_countdown(reentry);
    reentry.observe(observation(10, 0, 0));
    require(reentry.snapshot(root).state == sfr::GameplayState::invalid,
            "phase 10 reentry after racing begins invalidates the run");

    sfr::RaceGameplayProgress exited;
    prestart(exited);
    finish_countdown(exited);
    exited.observe(observation(11, step_ms, 0));
    exited.observe(observation(12, step_ms, 0));
    require(exited.snapshot(root).state == sfr::GameplayState::outside &&
            exited.snapshot(root).tick == 301,
            "leaving running phase becomes outside without a tick");
    exited.observe(observation(11, step_ms, 0));
    require(exited.snapshot(root).state == sfr::GameplayState::invalid,
            "phase reentry in the same epoch invalidates");

    sfr::RaceGameplayProgress limited;
    prestart(limited);
    finish_countdown(limited);
    auto sample = limited.snapshot(root);
    require(sfr::gameplay_limit_reached(sample, 300) &&
            !sfr::gameplay_limit_reached(sample, 301) && !sfr::gameplay_limit_reached(sample, 0),
            "the limit uses combined updates including the countdown");
    limited.observe(observation(11, step_ms, 0));
    sample = limited.snapshot(root);
    require(sfr::gameplay_limit_reached(sample, 301), "combined limit advances into race updates");
    require(limited.snapshot(root + 4).state == sfr::GameplayState::outside &&
            !sfr::gameplay_limit_reached(limited.snapshot(root + 4), 1),
            "non-live owner snapshots cannot trigger a limit");
    auto invalid = sample;
    invalid.state = sfr::GameplayState::invalid;
    require(!sfr::gameplay_limit_reached(invalid, 1), "invalid snapshots cannot trigger a limit");

    std::ostringstream output;
    sfr::write_gameplay_progress(output, sample);
    const std::string line = output.str();
    require(line.find("gameplay_schema=1 game_contract=solo_fixed_v1") != std::string::npos &&
            line.find("game_stage=racing game_start_observed=1") != std::string::npos &&
            line.find("game_tick=301 game_countdown_tick=300 game_race_tick=1") != std::string::npos &&
            line.find("game_countdown_ms=0") != std::string::npos &&
            line.find("game_config=3:1:7:0:2:4:9") != std::string::npos,
            "output reports observed combined and stage counters with raw configuration");
}
}

int main() {
    try {
        epoch_and_countdown_boundary();
        combined_long_run_and_pause_gates();
        late_start_unknown_phases_and_invalid_updates_fail_closed();
        identity_exit_limits_and_output();
        std::cout << "Race gameplay progress checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
