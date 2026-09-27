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

// A player who names the controller they want gets that one, wherever the
// host lists it -- which is the whole point of naming it.
void a_player_can_ask_for_one_controller() {
    sfr::PadAssignment pads;
    pads.prefer(0, sony);
    pads.update(std::vector<uint64_t>{0, sony});
    require(pads.pad_of(0) == sony, "the controller asked for is theirs");
    require(pads.pad_of(1) == 0, "and the other one goes to the next player");

    // Taken off whoever else had it.
    sfr::PadAssignment taken;
    taken.update(std::vector<uint64_t>{0, sony});
    require(taken.pad_of(0) == 0, "the host's order to begin with");
    taken.prefer(1, 0);
    taken.update(std::vector<uint64_t>{0, sony});
    require(taken.pad_of(1) == 0 && taken.pad_of(0) == sony, "asking for it moves it, and the first player takes the other");

    // While it is unplugged that player waits rather than taking a stranger's.
    sfr::PadAssignment waiting;
    waiting.prefer(0, sony);
    waiting.update(std::vector<uint64_t>{0});
    require(waiting.pad_of(0) == sfr::PadAssignment::no_pad, "a player who asked for one that is not here waits");
    require(waiting.pad_of(1) == 0, "and the pad that is here belongs to somebody who did not ask");
    waiting.update(std::vector<uint64_t>{0, sony});
    require(waiting.pad_of(0) == sony, "and takes it the moment it arrives");

    // Nobody else is given a controller somebody has asked for.
    sfr::PadAssignment reserved;
    reserved.prefer(1, sony);
    reserved.update(std::vector<uint64_t>{sony});
    require(reserved.pad_of(0) == sfr::PadAssignment::no_pad && reserved.pad_of(1) == sony,
            "a controller somebody asked for is not handed to anybody else");

    // Taking the request back does not pull the controller out of that
    // player's hands: a controller keeps its number while it stays
    // connected. It goes back to the host's order once it is unplugged.
    reserved.prefer(1, sfr::PadAssignment::no_pad);
    reserved.update(std::vector<uint64_t>{sony});
    require(reserved.pad_of(1) == sony, "the controller stays with the player holding it");
    reserved.update(std::vector<uint64_t>{});
    reserved.update(std::vector<uint64_t>{sony});
    require(reserved.pad_of(0) == sony, "and after a reconnection it is the host's order again");
}

void disabled_slots_do_not_receive_or_reserve_controllers() {
    sfr::PadAssignment pads;
    pads.enable(0, false);
    pads.update(std::vector<uint64_t>{sony});
    require(pads.pad_of(0) == sfr::PadAssignment::no_pad && pads.pad_of(1) == sony,
            "a keyboard-only first slot leaves the lone pad for automatic player two");
    pads.prefer(0, sony);
    pads.update(std::vector<uint64_t>{sony});
    require(pads.pad_of(1) == sony, "a disabled slot's stale preference does not steal or reserve a pad");

    sfr::PadAssignment changed;
    changed.prefer(0, sony);
    changed.update(std::vector<uint64_t>{sony});
    changed.enable(0, false);
    changed.update(std::vector<uint64_t>{sony});
    require(changed.pad_of(0) == sfr::PadAssignment::no_pad && changed.pad_of(1) == sony,
            "disabling an assigned slot releases its pad to another enabled slot");
    changed.enable(0, true);
    changed.update(std::vector<uint64_t>{sony});
    require(changed.pad_of(0) == sony, "re-enabling the slot restores its stored preference");
    changed.enable(0, false);
    changed.enable(1, false);
    changed.update(std::vector<uint64_t>{sony});
    require(changed.pad_of(0) == sfr::PadAssignment::no_pad && changed.pad_of(1) == sfr::PadAssignment::no_pad,
            "multiple disabled slots remain unassigned");
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
        a_player_can_ask_for_one_controller();
        disabled_slots_do_not_receive_or_reserve_controllers();
        only_four_can_play();
        std::cout << "Pad assignment checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
