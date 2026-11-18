#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

// CPU difficulty (Issue #420): how fast the computer racers ride in a Free
// Race. The launcher's General > Gameplay > CPU difficulty passes it to the
// game as SFR_CPU_DIFFICULTY (newbie, easy, normal or hard; Normal, the
// original game, when it is missing or unknown).
//
// How the game drives a computer racer: its controller (class vtable
// 821A1EF0, at racer +0xC94; the controller's racer is at +0xC0) runs
// 8229F1C0 once per update with the racer's rider (racer +0x60). That call
// sets the rider's top speed (+0x724) from the computer's level and how far
// it is behind its schedule. The riding states only accelerate the current
// speed (+0x71C) up to the top, or the boost top (+0x72C) while boosting;
// slopes, boosts and rings push it further, and the only brake above the top
// is 1/6 km/h a call at the end of 8229F1C0. In a Free Race, when player 0 is
// near (within 100 units), the same call gives the computer the player's top
// speed (player +0x784) instead: 30 km/h less while it follows (controller
// state +0x754 of 1 or 2), 20 km/h more while it passes (state 3). That is
// what keeps the original computers right at the player's back.
//
// Newbie and Easy cap the computer's current speed at a ratio of its top
// speed after each 8229F1C0 call, which takes away the extra speed from
// slopes, boosts and rings. Hard changes the near-player rule: the computer
// follows at the player's own top speed and passes at 50 km/h more. Measured
// on Wild Canyon (Free Race, 60 Hz, an unattended player), the player's
// median place was 1 with Newbie, 3 with Easy, 5 with Normal and 8 with Hard.
//
// Only computer racers in an offline Free Race change: story and the other
// modes, players, and online races (SFR_LIVE_HOST, SFR_LIVE_SERVER) keep the
// original game.
namespace sfr::cpu_difficulty {

enum class Level : uint8_t { newbie, easy, normal, hard };

inline constexpr Level levels[] = {Level::newbie, Level::easy, Level::normal, Level::hard};

// The name in settings.ini and SFR_CPU_DIFFICULTY.
constexpr const char* name(Level level) {
    switch (level) {
    case Level::newbie: return "newbie";
    case Level::easy: return "easy";
    case Level::hard: return "hard";
    default: return "normal";
    }
}

// Whether text names a level; level is set only then.
constexpr bool parse(std::string_view text, Level& level) {
    for (const Level candidate : levels)
        if (text == name(candidate)) {
            level = candidate;
            return true;
        }
    return false;
}

// What the game runs with: Normal unless the variable names another level.
constexpr Level from_environment(const char* value) {
    Level level = Level::normal;
    if (value) parse(value, level);
    return level;
}

// The game's speeds are in units of 216 km/h: 1 km/h is 1/216 of a unit.
inline constexpr float km_per_hour = 1.0f / 216.0f;

// Newbie and Easy: the largest current speed a computer racer keeps, as a
// ratio of its top speed (the larger of +0x724 and the boost top +0x72C). 0
// leaves the speed alone. The original computers ride above their top speed
// much of the time, so 1.15 still lets them keep up with a player, while 1.08
// leaves them behind one who rides well.
constexpr float speed_cap_ratio(Level level) {
    switch (level) {
    case Level::newbie: return 1.08f;
    case Level::easy: return 1.15f;
    default: return 0.0f;
    }
}

// The rider's state (+0x948) in which the cap applies: 7, riding along the
// course under the racer's own control. Only in that state does 8229F1C0 set
// the top speed from the computer's level and schedule (8229F4C0, 8229F574),
// so only there is +0x724 a top speed to compare with. In the other states it
// is left at whatever it was: state 0, for one, is a racer carried by a course
// object (822A9F58), which moves it at the racer's own speed (Forgotten Tomb's
// launcher after the falling pillars, the start's run-in). Capping there held
// a Newbie computer at the launcher's entry for the rest of the race, so the
// cap leaves every other state alone.
inline constexpr uint32_t riding_state = 7;

constexpr bool caps_in_state(uint32_t rider_state) { return rider_state == riding_state; }

// The current speed after the cap: only ever lowered, never raised, and left
// alone without a cap or a top speed.
constexpr float capped_speed(float speed, float top, float boost_top, float ratio) {
    if (!(ratio > 0.0f)) return speed;
    const float cap = std::max(top, boost_top) * ratio;
    return cap > 0.0f && speed > cap ? cap : speed;
}

// The race kinds that change: a Free Race (race +0x9C, 0) under the usual
// rules (the rules' +0xC; 2 and 3 are the rule sets the game itself gives
// other constants here), and not online.
constexpr bool applies(uint32_t race_type, uint32_t rule, bool online) {
    return !online && race_type == 0 && rule != 2 && rule != 3;
}

// The controller states of the near-player rule: 1 and 2 follow, 3 passes.
constexpr bool following(uint32_t state) { return state == 1 || state == 2; }
constexpr bool passing(uint32_t state) { return state == 3; }

// Hard: how much faster than the player's top speed a computer near the
// player rides (the original is -30 km/h following, +20 passing).
inline constexpr float hard_follow = 0.0f * km_per_hour;
inline constexpr float hard_pass = 50.0f * km_per_hour;

// The top speed the near-player rule gives a computer: with Hard, the
// player's top plus hard_follow or hard_pass; otherwise what the game
// computed (game_top).
constexpr float near_player_top(Level level, uint32_t state, float player_top, float game_top) {
    if (level != Level::hard) return game_top;
    if (following(state)) return player_top + hard_follow;
    if (passing(state)) return player_top + hard_pass;
    return game_top;
}

// Hard, away from the player. The near-player rule above only runs within its
// radius (about 100 units): a player who breaks away leaves every computer to
// its own level and schedule, which is Normal. So with Hard, a riding computer
// (rider state 7) outside the radius and behind player 0 in the race order
// (its place, +0x358 of its progress entry, after the player's) gets at least
// the player's top speed plus hard_far_margin, the 20 km/h the original gives
// a computer passing the player. It is applied where the game is done with
// the top speed and before its brake (8229F764, so the brake and the state-3
// boost that follow see it), as the near rule's value is.
//
// This was chosen over scaling the game's own behind-schedule catch-up (the
// offset at controller +0x798 that 822A2900 adds to the level's speed): that
// catch-up follows the course's reference schedule, not the player, and
// 822A2900 clamps its result at 0.926 (200 km/h), so a fast player stays out
// of reach whatever it is scaled by. The player's own top speed is what the
// near rule already uses. It only raises the top, never lowers it, so a
// computer that is ahead, or already faster, is not touched; positions are
// never changed.
inline constexpr float hard_far_margin = 20.0f * km_per_hour;

constexpr float far_behind_top(Level level, bool near_player, uint32_t place, uint32_t player_place, float player_top,
                               float game_top) {
    if (level != Level::hard || near_player || place <= player_place) return game_top;
    return std::max(game_top, player_top + hard_far_margin);
}

}  // namespace sfr::cpu_difficulty
