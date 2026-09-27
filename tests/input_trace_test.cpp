#include "input_trace.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void alternating_players_do_not_repeat_unchanged_states() {
    sfr::InputTrace trace;
    // Issue #2 alternates the connected player's unchanged packet 62 with
    // player 2's not-connected result every frame: these are only two states.
    unsigned lines = 0;
    for (unsigned frame = 0; frame < 600; ++frame) {
        lines += trace.changed(0, 0, 62);
        lines += trace.changed(1, 0x48f, 0);
    }
    require(lines == 2, "alternating players must log each unchanged state only once");
}

void first_queries_and_connection_changes_are_visible() {
    sfr::InputTrace trace;
    for (unsigned user = 0; user < 4; ++user) {
        require(trace.changed(user, 0x48f, 0), "each player's first query is logged");
        require(!trace.changed(user, 0x48f, 0), "repeated disconnection is quiet");
        require(trace.changed(user, 0, 0), "connection with packet zero is logged");
        require(!trace.changed(user, 0, 0), "unchanged connected state is quiet");
        require(trace.changed(user, 0x48f, 0), "disconnection with packet zero is logged");
    }
}

void packet_changes_belong_to_one_player() {
    sfr::InputTrace trace;
    require(trace.changed(0, 0, 7) && trace.changed(1, 0, 7), "identical initial packets belong to different players");
    require(trace.changed(1, 0, 8), "a real input change is logged");
    require(!trace.changed(0, 0, 7), "another player's input does not change this player's trace");
    require(!trace.changed(1, 0, 8), "new packet is logged once");
    require(trace.changed(0, 0, 0xffffffffu), "full-width packet is retained");
    require(trace.changed(0, 0, 0), "packet wraparound is a change");
}
}

int main() {
    try {
        alternating_players_do_not_repeat_unchanged_states();
        first_queries_and_connection_changes_are_visible();
        packet_changes_belong_to_one_player();
        std::cout << "Input trace checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
