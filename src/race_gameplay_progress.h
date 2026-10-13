#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <ostream>

namespace sfr {
enum class GameplayState { unseen, unsupported, prestart, starting, advancing, paused, outside, invalid };
enum class GameplayStage { none, countdown, racing };
struct GameplayObservation {
    bool supported = false, paused = false;
    uint32_t object = 0, controller = 0, phase = 0;
    double elapsed_ms = 0;
    double countdown_ms = 5000;
    std::array<uint32_t, 7> config{};
};
struct GameplayProgress {
    GameplayState state = GameplayState::unseen;
    uint64_t run = 0;
    uint32_t object = 0, controller = 0, phase = 0, tick = 0;
    uint32_t countdown_tick = 0, race_tick = 0;
    double elapsed_ms = 0;
    double countdown_ms = 5000;
    bool start_observed = false;
    GameplayStage stage = GameplayStage::none;
    std::array<uint32_t, 7> config{};
    const char* reason = "unseen";
};

// Restricted contract: known controller/mode, one local rider, ordinary fixed
// 60 Hz clock. Source: 8224B750 phase10 is countdown, phase11 calls8224A858;
// timer+48/current+40 is copied bit-for-bit into the normal rider HUD record.
// All300 countdown updates are retained. The final countdown update publishes
// phase11/elapsed0; it still belongs to countdown, not racing tick1.
// This policy observes completed original updates. It never advances the game.
class RaceGameplayProgress {
public:
    void constructed(uint32_t object) {
        progress_ = {};
        progress_.run = ++generation_;
        progress_.object = object;
        saw_prestart_ = bound_ = started_ = ended_ = running_entered_ = false;
    }
    void observe(const GameplayObservation& value) {
        if (!progress_.run) return; // Late attachment cannot invent an epoch.
        if (progress_.state == GameplayState::invalid) return;
        if (value.object != progress_.object) { fail("object_changed"); return; }
        if (!value.supported) {
            if (bound_) fail("contract_changed");
            else { progress_.state = GameplayState::unsupported; progress_.reason = "unsupported"; }
            return;
        }
        if (!bound_) {
            bound_ = true;
            progress_.controller = value.controller;
            progress_.config = value.config;
        } else if (value.controller != progress_.controller || value.config != progress_.config) {
            fail("identity_changed"); return;
        }
        if (!std::isfinite(value.elapsed_ms) || value.elapsed_ms < 0 ||
            !std::isfinite(value.countdown_ms) || value.countdown_ms < 0 || value.countdown_ms > 5000.001) {
            fail("invalid_time"); return;
        }
        if (value.phase > 16) { fail("unknown_phase"); return; }
        progress_.phase = value.phase;
        if (value.phase < 10 || value.phase > 11) {
            if (started_ || value.phase > 11) {
                ended_ = true;
                progress_.state = GameplayState::outside;
                progress_.reason = "left_running_phase";
            } else {
                saw_prestart_ = true;
                progress_.state = GameplayState::prestart;
                progress_.reason = "prestart";
            }
            progress_.elapsed_ms = value.elapsed_ms;
            return;
        }
        if (ended_ || (running_entered_ && value.phase == 10)) { fail("phase_reentry"); return; }
        if (!started_) {
            if (!saw_prestart_ || value.phase != 10 || std::fabs(value.elapsed_ms) > .001) {
                fail("start_not_observed"); return;
            }
            started_ = true;
            progress_.start_observed = true;
            progress_.countdown_ms = 5000;
            progress_.elapsed_ms = 0;
        }
        if (!running_entered_) {
            if (std::fabs(value.elapsed_ms) > .001) { fail("race_time_during_countdown"); return; }
            const double delta = progress_.countdown_ms - value.countdown_ms;
            progress_.stage = GameplayStage::countdown;
            if (value.paused) {
                if (std::fabs(delta) > .001) { fail("paused_time_changed"); return; }
                progress_.state = GameplayState::paused;
                progress_.reason = "pause_gate";
            } else {
                if (std::fabs(delta - 1000.0 / 60.0) > .001) { fail("non_unit_countdown_step"); return; }
                ++progress_.tick;
                ++progress_.countdown_tick;
                progress_.state = GameplayState::advancing;
                progress_.reason = "countdown_step";
            }
            progress_.countdown_ms = value.countdown_ms;
            if (value.phase == 11) {
                if (progress_.countdown_tick != 300 || value.countdown_ms > .001) {
                    fail("incomplete_countdown"); return;
                }
                running_entered_ = true;
            } else if (progress_.countdown_tick >= 300) {
                fail("countdown_phase_not_finished");
            }
            return;
        }
        if (value.countdown_ms > .001) { fail("countdown_restarted"); return; }
        const double delta = value.elapsed_ms - progress_.elapsed_ms;
        progress_.stage = GameplayStage::racing;
        if (value.paused) {
            if (std::fabs(delta) > .001) { fail("paused_time_changed"); return; }
            progress_.state = GameplayState::paused;
            progress_.reason = "pause_gate";
        } else {
            // Fixed-step float milliseconds is about16.66666794. Verify the
            // update, rather than rounding away skipped/extra simulation.
            if (std::fabs(delta - 1000.0 / 60.0) > .001) { fail("non_unit_step"); return; }
            ++progress_.tick;
            ++progress_.race_tick;
            progress_.state = GameplayState::advancing;
            progress_.reason = "unit_step";
        }
        progress_.elapsed_ms = value.elapsed_ms;
    }
    GameplayProgress snapshot(uint32_t live_object) const {
        auto sample = progress_;
        if (sample.run && live_object != sample.object) {
            sample.state = GameplayState::outside;
            sample.reason = "object_not_live";
        }
        return sample;
    }
private:
    void fail(const char* reason) { progress_.state = GameplayState::invalid; progress_.reason = reason; }
    uint64_t generation_ = 0;
    GameplayProgress progress_{};
    bool saw_prestart_ = false, bound_ = false, started_ = false, ended_ = false, running_entered_ = false;
};

inline const char* gameplay_state_name(GameplayState state) {
    switch (state) {
    case GameplayState::unseen: return "unseen";
    case GameplayState::unsupported: return "unsupported";
    case GameplayState::prestart: return "prestart";
    case GameplayState::starting: return "starting";
    case GameplayState::advancing: return "advancing";
    case GameplayState::paused: return "paused";
    case GameplayState::outside: return "outside";
    case GameplayState::invalid: return "invalid";
    }
    return "invalid";
}
inline bool gameplay_limit_reached(const GameplayProgress& sample, uint32_t limit) {
    return limit && sample.state == GameplayState::advancing && sample.tick >= limit;
}
inline void write_gameplay_progress(std::ostream& out, const GameplayProgress& sample) {
    const auto old_precision = out.precision(12);
    out << " gameplay_schema=1 game_contract=solo_fixed_v1 game_run=" << sample.run
        << " game_state=" << gameplay_state_name(sample.state) << " game_reason=" << sample.reason
        << " game_stage=" << (sample.stage == GameplayStage::countdown ? "countdown" :
                               sample.stage == GameplayStage::racing ? "racing" : "none")
        << " game_start_observed=" << sample.start_observed
        << " game_phase=" << sample.phase << " game_tick=" << sample.tick
        << " game_countdown_tick=" << sample.countdown_tick << " game_race_tick=" << sample.race_tick
        << " game_countdown_ms=" << sample.countdown_ms
        << " game_elapsed_ms=" << sample.elapsed_ms << " game_config=";
    for (unsigned i = 0; i < sample.config.size(); ++i)
        out << (i ? ":" : "") << sample.config[i];
    out.precision(old_precision);
}

// Runtime publication is synchronized once per owner update/present. Normal
// play takes no new guest reads or locks when the opt-in switch/limit is absent.
bool gameplay_progress_enabled();
GameplayProgress gameplay_progress_snapshot(uint32_t live_object);
}
