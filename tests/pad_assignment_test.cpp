#include "pad_assignment.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

constexpr uint64_t sony = 100;  // how native_input names a PlayStation pad

void the_first_pad_is_player_one() {
    sfr::PadAssignment pads;
    const std::vector<uint64_t> only_sony = {sony};
    pads.update(only_sony);
    require(pads.pad_of(0) == sony, "the one pad connected plays as player one");
    require(pads.pad_of(1) == sfr::PadAssignment::no_pad, "and nobody else is playing");
}

void a_second_pad_joins_beside_the_first() {
    sfr::PadAssignment pads;
    const std::vector<uint64_t> only_sony = {sony};
    pads.update(only_sony);
    // An XInput pad plugged in afterwards takes XInput slot 0, which is the
    // lowest free one -- but the Sony pad is already player one.
    const std::vector<uint64_t> both = {0, sony};
    pads.update(both);
    require(pads.pad_of(0) == sony, "the pad being played with keeps player one");
    require(pads.pad_of(1) == 0, "the new one joins as player two");
    require(pads.player_of(0) == 1 && pads.player_of(sony) == 0, "and each knows which it is");
}

void a_pad_that_goes_gives_its_number_back() {
    sfr::PadAssignment pads;
    const std::vector<uint64_t> three = {0, 1, sony};
    pads.update(three);
    require(pads.pad_of(0) == 0 && pads.pad_of(1) == 1 && pads.pad_of(2) == sony, "three players, in the host's order");
    const std::vector<uint64_t> without_the_middle = {0, sony};
    pads.update(without_the_middle);
    require(pads.pad_of(0) == 0 && pads.pad_of(2) == sony, "the others do not move up");
    require(pads.pad_of(1) == sfr::PadAssignment::no_pad, "the number it had is free");
    const std::vector<uint64_t> back = {0, 1, sony};
    pads.update(back);
    require(pads.pad_of(1) == 1, "and the next pad to arrive takes it");
}

void nothing_moves_when_nothing_changes() {
    sfr::PadAssignment pads;
    const std::vector<uint64_t> two = {sony, 0};
    for (int again = 0; again < 5; ++again) pads.update(two);
    require(pads.pad_of(0) == sony && pads.pad_of(1) == 0, "asking again does not renumber anyone");
}

void only_four_can_play() {
    sfr::PadAssignment pads;
    const std::vector<uint64_t> five = {0, 1, 2, 3, sony};
    pads.update(five);
    require(pads.player_of(sony) == std::nullopt, "the fifth pad is not a player");
    require(pads.pad_of(3) == 3, "the first four are");
}
}

int main() {
    try {
        the_first_pad_is_player_one();
        a_second_pad_joins_beside_the_first();
        a_pad_that_goes_gives_its_number_back();
        nothing_moves_when_nothing_changes();
        only_four_can_play();
        std::cout << "Pad assignment checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
