#include "cpu_difficulty.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using namespace sfr::cpu_difficulty;

}  // namespace

int main() try {
    // The names round-trip; a missing or unknown value is Normal, the original game.
    for (const Level level : levels) {
        Level read = Level::normal;
        require(parse(name(level), read) && read == level, "every level reads back from its name");
        require(from_environment(name(level)) == level, "the game reads every level the launcher passes");
    }
    require(from_environment(nullptr) == Level::normal, "without SFR_CPU_DIFFICULTY the game is the original");
    require(from_environment("") == Level::normal && from_environment("Hard") == Level::normal &&
                from_environment("1") == Level::normal,
            "an unknown value is Normal");
    Level untouched = Level::hard;
    require(!parse("insane", untouched) && untouched == Level::hard, "an unknown name leaves the level alone");

    // Only Newbie and Easy cap the speed, Newbie the tighter.
    require(speed_cap_ratio(Level::normal) == 0.0f && speed_cap_ratio(Level::hard) == 0.0f,
            "Normal and Hard leave the speed alone");
    require(speed_cap_ratio(Level::newbie) == 1.08f && speed_cap_ratio(Level::easy) == 1.15f,
            "Newbie caps at 1.08 times the top speed, Easy at 1.15");

    // The cap: never raises a speed, lowers one above ratio times the larger top.
    const float top = 0.8f, boost = 1.0f;
    require(capped_speed(0.5f, top, 0.0f, 1.08f) == 0.5f, "a speed under the cap stays");
    require(capped_speed(2.0f, top, 0.0f, 1.08f) == top * 1.08f, "a speed over the cap is lowered to it");
    require(capped_speed(1.05f, top, boost, 1.08f) == 1.05f, "while boosting the boost top counts");
    require(capped_speed(2.0f, top, boost, 1.15f) == boost * 1.15f, "the cap follows the larger top");
    require(capped_speed(2.0f, top, boost, 0.0f) == 2.0f, "without a cap the speed stays");
    require(capped_speed(2.0f, 0.0f, 0.0f, 1.08f) == 2.0f, "without a top speed the speed stays");
    require(capped_speed(-0.3f, top, 0.0f, 1.08f) == -0.3f, "a speed backwards is not touched");

    // Only while riding on its own (rider state 7): a racer a course object carries (0) keeps its speed.
    require(caps_in_state(7), "a computer riding along the course is capped");
    require(!caps_in_state(0) && !caps_in_state(1) && !caps_in_state(8), "other rider states are not capped");

    // Where it applies: an offline Free Race under the usual rules.
    require(applies(0, 0, false) && applies(0, 1, false), "a Free Race changes");
    require(!applies(0, 2, false) && !applies(0, 3, false), "the other rule sets keep the original");
    require(!applies(1, 0, false) && !applies(2, 0, false) && !applies(3, 0, false), "other race kinds keep the original");
    require(!applies(0, 1, true), "online races keep the original");

    // The near-player rule: the original computes player top -30 km/h (follow) and +20 km/h (pass).
    const float player = 0.9f;
    const float follow = player - 30 * km_per_hour, pass = player + 20 * km_per_hour;
    for (const Level level : {Level::newbie, Level::easy, Level::normal}) {
        require(near_player_top(level, 1, player, follow) == follow && near_player_top(level, 3, player, pass) == pass,
                "only Hard changes the near-player rule");
    }
    require(near_player_top(Level::hard, 1, player, follow) == player &&
                near_player_top(Level::hard, 2, player, follow) == player,
            "Hard: a following computer gets the player's top speed");
    require(near_player_top(Level::hard, 3, player, pass) == player + 50 * km_per_hour,
            "Hard: a passing computer gets 50 km/h more than the player");
    require(near_player_top(Level::hard, 0, player, 0.7f) == 0.7f && near_player_top(Level::hard, 7, player, 0.7f) == 0.7f,
            "Hard: other controller states keep what the game computed");
    require(near_player_top(Level::hard, 1, player, follow) > follow &&
                near_player_top(Level::hard, 3, player, pass) > pass,
            "Hard is faster than the original in both states");

    std::cout << "cpu_difficulty: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "cpu_difficulty: " << error.what() << '\n';
    return 1;
}
