#pragma once

#include <cstdint>

// Super Sonic on his board, without the flying form (Issue #379).
//
// With the Chaos Emeralds gear, Sonic turns into Super Sonic at 30 rings
// (82296348) and his ability object (class 821A52F0, update 823D9970, phase
// at +20) takes over. Its phases: 0 -> 1 -> 2 (transforming) -> 3 -> 4, Super
// Sonic riding his board. In phase 4 a kick dash (animation 67 held past 7.0,
// the shared check 823D8EE8) starts the flying form: animation 164, then 5,
// then 7 with the flight loop 165. While the racer's byte +2732 says he flies,
// 82296348 takes a ring every 20 frames (a third of a second), and he can't
// grab poles; he lands back in phase 4 when the flight ends. Before the phase switch, every phase from 3
// on checks the rings (823D9730): at 0 Super Sonic turns back into Sonic.
//
// Phase 3 only moves on to phase 4. Running a phase-4 update as phase 3 keeps
// the ring check and leaves out the start of the flight, so the board form
// stays and the kick dash is an ordinary one.
namespace sfr::super_sonic {

inline constexpr uint32_t phase_settling = 3;
inline constexpr uint32_t phase_on_board = 4;

// The phase the ability update runs with.
constexpr uint32_t update_phase(uint32_t phase, bool stay_on_board) {
    return stay_on_board && phase == phase_on_board ? phase_settling : phase;
}

// All three types on the board (Issue #393).
//
// A racer's live gear (racer +3216) keeps its type mask at +168: 0x1 speed
// (rails, 822B1818), 0x2 fly (fly rings, 822B3610), 0x4 power (breakable
// objects, 822BE420). The Omnipotence gear (gear table 0x82B47E78, entry 23)
// has all three; the Chaos Emeralds (entry 38) have none, so Super Sonic can
// do none of it. The transformation and the return to Sonic (82296348) both
// rebuild the live gear (82297B90) from the table, which puts the mask back.
inline constexpr uint32_t type_speed = 0x1;
inline constexpr uint32_t type_fly = 0x2;
inline constexpr uint32_t type_power = 0x4;
inline constexpr uint32_t all_types = type_speed | type_fly | type_power;

// An environment switch is on when it is set and not empty (SFR_LIVE_HOST=1,
// SFR_LIVE_SERVER=host[:port]; the launcher passes "" for the one not used).
constexpr bool switch_set(const char* value) { return value && *value; }

// Both Super Sonic options change the race, so they stay off online, where
// every player has to race by the same rules.
constexpr bool options_apply(bool live_host, bool live_server) { return !live_host && !live_server; }

struct Options {
    bool stay_on_board = false;
    bool all_types = false;  // only together with stay_on_board
    bool online = false;
};

constexpr bool stays_on_board(const Options& options) { return options.stay_on_board && !options.online; }

// Whether the ability object's racer gets all three types after an update
// that left the ability in `phase`: only Super Sonic on his board (phases 3
// and 4; at 0 he has just turned back into Sonic), only a player's racer, not
// a computer one.
constexpr bool gets_all_types(const Options& options, uint32_t phase, bool local_player) {
    return stays_on_board(options) && options.all_types && local_player &&
           (phase == phase_settling || phase == phase_on_board);
}

constexpr uint32_t type_mask(uint32_t mask, bool all) { return all ? mask | all_types : mask; }

}  // namespace sfr::super_sonic
