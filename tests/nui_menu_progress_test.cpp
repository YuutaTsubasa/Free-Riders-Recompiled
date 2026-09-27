#include "nui_menu_progress.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    sfr::NuiMenuProgress progress;
    sfr::NuiMenuActions actions{{{1, 0, 7}, {0, 0, 0}}};
    std::vector<unsigned> calls;
    uint32_t manager = 1, serviced = 0, advancing = 0;
    bool second = false, enabled = true, drop_second = false;
    bool frame(bool original_progress = false, bool pulse = false) {
        const auto before = actions;
        if (original_progress) for (auto& action : actions) if (action.type) ++action.elapsed;
        for (unsigned player=0; player<2; ++player)
            if (advancing & (1u << player)) ++actions[player].elapsed;
        const auto after = actions;
        bool consumed = false;
        progress.update(manager, before, after, [&] { return second; }, [&](unsigned player) {
            calls.push_back(player);
            if (actions[player].type) ++actions[player].elapsed;
            if (player == 1 && pulse) consumed = true;
            if (player == 0 && drop_second) second = false;
        }, enabled, serviced);
        return consumed;
    }
    void frames(unsigned count) { while (count--) frame(); }
};
void one_player() {
    Fixture f;
    f.frames(29); require(f.calls.empty(), "wait 30 unchanged pending frames");
    f.frame(); require(f.calls == std::vector<unsigned>{0}, "one-player fallback updates only P1");
}
void continuous_recovery() {
    Fixture f; f.frames(30); f.calls.clear(); f.frame();
    require(f.calls == std::vector<unsigned>{0}, "fallback elapsed increments must not postpone the next update");
}
void both_players() {
    Fixture f; f.second = true;
    f.frames(30);
    require(f.calls == std::vector<unsigned>({0, 1}), "a P1 stall must service both guest-enabled players");
    require(f.frame(false, true), "continuous fallback must catch a one-frame P2 gesture after its own elapsed advance");
    require(f.calls == std::vector<unsigned>({0, 1, 0, 1}), "recovery must update both every stalled frame");
}
void second_only() {
    Fixture f; f.second = true; f.actions = {{{0, 0, 0}, {1, 0, 8}}};
    f.frames(30); require(f.calls == std::vector<unsigned>({0, 1}), "P2-only pending action must trigger both updates");
}
void normal_progress() {
    Fixture f; f.second = true; f.frames(29);
    f.frame(true); require(f.calls.empty(), "normal guest advancement at threshold must prevent duplicate fallback");
    f.frames(29); require(f.calls.empty(), "normal advancement resets the stall interval");
    f.frame(); require(f.calls.size() == 2, "fallback resumes only after a new stall interval");
    f.calls.clear(); f.frame(true); require(f.calls.empty(), "normal advancement stops active recovery immediately");
}
void busy_normal_service() {
    Fixture f; f.second = true; f.actions[1] = {1, 0, 8}; f.serviced = 3;
    f.frames(60);
    require(f.calls.empty(), "normal cursor service with unchanged busy action must not trigger fallback");
}
void busy_service_stops_recovery() {
    Fixture f; f.second = true; f.frames(30); f.calls.clear();
    f.serviced = 3; f.frame();
    require(f.calls.empty(), "normal service must stop active recovery even without elapsed progress");
    f.serviced = 0; f.frames(29);
    require(f.calls.empty(), "normal service resets the stall age");
}
void partial_service() {
    Fixture f; f.second = true; f.actions[1] = {1, 0, 8}; f.serviced = 1;
    f.frames(30);
    require(f.calls == std::vector<unsigned>{1}, "P1 normal service must allow only stalled P2 recovery");
    f.calls.clear(); f.serviced = 2; f.frames(30);
    require(f.calls == std::vector<unsigned>{0}, "P2 normal service must allow only stalled P1 recovery");
}
void partial_progress() {
    Fixture f; f.second = true; f.actions[1] = {1, 0, 8}; f.advancing = 1;
    f.frames(30);
    require(f.calls == std::vector<unsigned>{1}, "P1 action progress must not suppress P2 stall recovery");
}
void service_scope() {
    using Scope = sfr::NuiMenuServiceScope;
    Scope::record(1, 0); // outside a normal update, including recovery
    Scope outer(1);
    require(outer.serviced() == 0, "service outside the normal scope must not carry over");
    Scope::record(2, 0); Scope::record(1, 2);
    require(outer.serviced() == 0, "other managers and invalid players must not contaminate the scope");
    Scope::record(1, 0);
    try {
        Scope inner(2);
        Scope::record(2, 1);
        require(inner.serviced() == 2 && outer.serviced() == 1, "nested managers record their own players");
        throw 1;
    } catch (int) {}
    Scope::record(1, 1);
    require(outer.serviced() == 3, "nested scope restores its predecessor when unwinding");
}
void independent_gear_player() {
    // P1's confirmation has cleared its delayed action, but the manager
    // keeps waiting on its request for hundreds more frames.
    for (unsigned frame=0; frame<690; ++frame)
        require(sfr::nui_gear_service_mask(true,4,true,{58,58},2,1,0)==2,
                "P1 confirmation must not freeze ready P2 after delayed actions clear");
    require(sfr::nui_gear_service_mask(true,4,true,{58,58},1,2,0)==1,
            "P2 confirmation must not freeze ready P1");
    require(sfr::nui_gear_service_mask(true,4,true,{58,58},3,0,0)==0,
            "do not bypass an unidentified manager wait");
    require(sfr::nui_gear_service_mask(true,4,true,{58,58},0,3,0)==0,
            "both waiting players must remain blocked");
    require(sfr::nui_gear_service_mask(true,4,true,{58,58},2,1,2)==0,
            "never duplicate normal service");
    require(sfr::nui_gear_service_mask(true,4,true,{58,58},3,1,0)==2,
            "never update a player observed blocked this frame");
    for (unsigned state : {2u,3u,5u,6u})
        require(sfr::nui_gear_service_mask(true,state,true,{58,58},2,1,0)==0,
                "opening and page transitions retain their original gates");
    require(sfr::nui_gear_service_mask(false,4,true,{58,58},2,1,0)==0,
            "single player must stay unchanged");
    require(sfr::nui_gear_service_mask(true,4,false,{58,58},2,1,0)==0,
            "disabled cursors must stay disabled");
    require(sfr::nui_gear_service_mask(true,4,true,{56,58},2,1,0)==0,
            "other pages must stay unchanged");
    // Recovery runs first. Its callback can consume P2's frame or leave Gear;
    // independent service must re-read both the shared mask and manager state.
    require(sfr::nui_gear_service_mask(true,4,true,{58,58},2,1,3)==0,
            "action recovery must not be followed by a second P2 update");
    require(sfr::nui_gear_service_mask(true,5,true,{58,58},2,1,1)==0,
            "action recovery entering transition must stop independent P2 service");
    sfr::NuiMenuServiceScope scope(100);
    sfr::NuiMenuServiceScope::record_wait(100+36+1828,false);
    require(scope.blocked()==1,"failed P1 gate must be captured for its manager");
    sfr::NuiMenuServiceScope::record_wait(100+36+1828+88,true);
    require(scope.blocked()==1,"successful P2 gate must not mark it blocked");
    sfr::NuiMenuServiceScope::record_wait(100+36+1828+1,false);
    require(scope.blocked()==1,"unrelated slot must not contaminate the scope");
}
void lifecycle() {
    Fixture f; f.frames(29); f.actions[0] = {}; f.frame();
    f.actions[0] = {1, 0, 7}; f.frames(29); require(f.calls.empty(), "cleared action resets stall age");
    f.actions[0].command = 9; f.frames(29); require(f.calls.empty(), "replacement command resets stall age");
    f.actions[0].type = 2; f.frames(29); require(f.calls.empty(), "replacement type resets stall age");
    ++f.manager; f.frames(29); require(f.calls.empty(), "manager replacement resets stall age");
    f.frame(); require(f.calls.size() == 1, "new manager still recovers after its own threshold");
    f.calls.clear(); f.enabled = false; f.frames(40); require(f.calls.empty(), "disabled fallback must not run");
    f.enabled = true; f.frames(29); require(f.calls.empty(), "re-enabling starts a fresh interval");
    f.frame(); require(f.calls.size() == 1, "re-enabled fallback recovers after threshold");
}
void guest_flag() {
    Fixture f; f.second = true; f.drop_second = true; f.frames(30);
    require(f.calls == std::vector<unsigned>{0}, "recheck guest two-player flag after P1 changes it");
    Fixture g; g.actions = {{{0, 0, 0}, {1, 0, 8}}}; g.frames(40);
    require(g.calls.empty(), "inactive P2 action must not trigger recovery");
}
}
int main() {
    unsigned failed = 0;
    for (auto test : {one_player, continuous_recovery, both_players, second_only, normal_progress, busy_normal_service, busy_service_stops_recovery, partial_service, partial_progress, service_scope, independent_gear_player, lifecycle, guest_flag}) {
        try { test(); }
        catch (const std::exception& error) { ++failed; std::cerr << error.what() << '\n'; }
    }
    if (failed) return 1;
    std::cout << "Menu progress checks passed\n";
}
