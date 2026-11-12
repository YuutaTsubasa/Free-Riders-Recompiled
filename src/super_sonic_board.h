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

}  // namespace sfr::super_sonic
