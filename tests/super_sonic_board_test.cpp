#include "super_sonic_board.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using sfr::super_sonic::update_phase;

// The ability update's switch (823D9970): what each phase does with the
// option off. Phase 3 only moves on to phase 4; phase 4 can start a flight.
uint32_t next_phase(uint32_t phase, bool kick_dash) {
    switch (phase) {
    case 0: return 1;
    case 3: return 4;
    case 4: return kick_dash ? 5 : 4;
    default: return phase;
    }
}

}  // namespace

int main() try {
    // Off: every phase runs as it is, so a kick dash on the board still flies.
    for (uint32_t phase = 0; phase < 8; ++phase)
        require(update_phase(phase, false) == phase, "off, the original phases run unchanged");
    require(next_phase(update_phase(4, false), true) == 5, "off, a kick dash starts the flying form");

    // On: the board phase runs as the one before it, which leads back to the
    // board, kick dash or not.
    require(update_phase(4, true) == 3, "on, the board phase runs as phase 3");
    require(next_phase(update_phase(4, true), true) == 4, "on, a kick dash keeps Super Sonic on the board");
    require(next_phase(update_phase(4, true), false) == 4, "on, riding on stays on the board");

    // On: the other phases, among them the transformation (0 to 2) and a
    // flight that was already under way (5, 7), are left alone.
    for (uint32_t phase : {0u, 1u, 2u, 3u, 5u, 6u, 7u})
        require(update_phase(phase, true) == phase, "on, only the board phase changes");

    std::cout << "super_sonic_board: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "super_sonic_board: " << error.what() << '\n';
    return 1;
}
