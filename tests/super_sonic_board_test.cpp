#include "super_sonic_board.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using sfr::super_sonic::update_phase;
using sfr::super_sonic::Options;
using sfr::super_sonic::gets_all_types;
using sfr::super_sonic::type_mask;

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

    // Online (Issue #393): neither option applies, whatever the launcher says.
    using sfr::super_sonic::options_apply;
    using sfr::super_sonic::stays_on_board;
    using sfr::super_sonic::switch_set;
    require(options_apply(switch_set(nullptr), switch_set("")), "offline, the options apply");
    require(!options_apply(switch_set("1"), switch_set("")), "hosting a LIVE session, they do not");
    require(!options_apply(switch_set(nullptr), switch_set("192.168.1.2:3074")), "joining a server, they do not");
    require(stays_on_board(Options{true, false, false}), "offline, stay on the board applies");
    require(!stays_on_board(Options{true, true, true}), "online, Super Sonic flies as in the original");

    // All types (Issue #393): only with both options, offline, for a player's
    // Super Sonic on his board.
    const Options all{true, true, false};
    require(gets_all_types(all, 4, true), "on the board, a player's Super Sonic gets all types");
    require(gets_all_types(all, 3, true), "settling on the board, too");
    require(!gets_all_types(all, 4, false), "a computer Super Sonic keeps the gear's types");
    for (uint32_t phase : {0u, 1u, 2u, 5u, 6u, 7u})
        require(!gets_all_types(all, phase, true), "Sonic, the transformation and a flight keep the gear's types");
    require(!gets_all_types(Options{false, true, false}, 4, true), "all types needs stay on the board");
    require(!gets_all_types(Options{true, false, false}, 4, true), "off, the gear's types stay");
    require(!gets_all_types(Options{true, true, true}, 4, true), "online, the gear's types stay");

    // The Chaos Emeralds have no type (0); Omnipotence has all three (7).
    require(type_mask(0, true) == 7 && type_mask(0, false) == 0, "all types sets speed, fly and power");
    require(type_mask(0x10, true) == 0x17, "other bits of the mask stay");

    // Rails (Issue #393): the rail check runs as for Sonic only for a
    // player's board Super Sonic with the speed type from "all types".
    using sfr::super_sonic::rail_check_as_sonic;
    using sfr::super_sonic::super_sonic_flag;
    const uint32_t super_flags = 0x00940002;  // board Super Sonic's racer +2220, logged on Dolphin Resort
    require(super_flags & super_sonic_flag, "the logged flags carry the Super Sonic bit");
    require(rail_check_as_sonic(all, true, 0x7, super_flags), "a player's board Super Sonic with all types can take rails");
    require(!rail_check_as_sonic(all, false, 0x7, super_flags), "a computer Super Sonic cannot");
    require(!rail_check_as_sonic(all, true, 0x0, super_flags), "without the speed bit (the gear's own types), he cannot");
    require(!rail_check_as_sonic(all, true, 0x7, 0x00800010), "not Super Sonic: the check runs as it is");
    require(!rail_check_as_sonic(Options{true, false, false}, true, 0x7, super_flags), "all types off: no rails");
    require(!rail_check_as_sonic(Options{false, true, false}, true, 0x7, super_flags), "stay on the board off: no rails");
    require(!rail_check_as_sonic(Options{true, true, true}, true, 0x7, super_flags), "online: no rails");

    std::cout << "super_sonic_board: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "super_sonic_board: " << error.what() << '\n';
    return 1;
}
