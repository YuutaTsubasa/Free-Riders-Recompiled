#pragma once
#include <array>
#include <cstdint>

namespace sfr {
// A player's outstanding confirmation must not suspend the other Gear page.
// Require an observed failed guest gate; unrelated waits and transitions stay
// under guest control. Never service a blocked or already updated player.
inline uint32_t nui_gear_service_mask(bool two_pads, uint32_t state, bool cursors_enabled,
        std::array<uint32_t,2> kinds, uint32_t settled, uint32_t blocked, uint32_t serviced) {
    if (!two_pads || state!=4 || !cursors_enabled || kinds[0]!=58 || kinds[1]!=58 || !blocked) return 0;
    return settled & ~blocked & ~serviced & 3u;
}
struct NuiMenuAction {
    uint32_t type = 0, elapsed = 0, command = 0;
    bool operator==(const NuiMenuAction&) const = default;
};
using NuiMenuActions = std::array<NuiMenuAction, 2>;

// Lives only around an original manager update. Recovery runs after this
// scope ends, so its own calls cannot count as normal service next frame.
class NuiMenuServiceScope {
public:
    explicit NuiMenuServiceScope(uint32_t manager) : manager_(manager), previous_(current_) { current_ = this; }
    ~NuiMenuServiceScope() { current_ = previous_; }
    NuiMenuServiceScope(const NuiMenuServiceScope&) = delete;
    NuiMenuServiceScope& operator=(const NuiMenuServiceScope&) = delete;
    uint32_t blocked() const { return blocked_; }
    static void record_wait(uint32_t slot, bool settled) {
        if (settled) return;
        for (auto* scope=current_; scope; scope=scope->previous_)
            for (uint32_t player=0; player<2; ++player)
                if (uint64_t(slot)==uint64_t(scope->manager_)+36+1828+88*player)
                    scope->blocked_ |= 1u<<player;
    }
    uint32_t serviced() const { return serviced_; }
    static void record(uint32_t manager, uint32_t player) {
        if (player >= 2) return;
        for (auto* scope = current_; scope; scope = scope->previous_)
            if (scope->manager_ == manager) scope->serviced_ |= 1u << player;
    }
private:
    uint32_t manager_, serviced_ = 0, blocked_ = 0;
    NuiMenuServiceScope* previous_;
    static inline thread_local NuiMenuServiceScope* current_ = nullptr;
};

// Observe only progress made by the original menu update. Progress from our
// recovery callback must not restart the 30-frame wait on the next frame.
class NuiMenuProgress {
public:
    template<class SecondEnabled, class Update>
    bool update(uint32_t manager, const NuiMenuActions& before, const NuiMenuActions& after,
                SecondEnabled second_enabled, Update advance, bool enabled, uint32_t serviced) {
        if (!enabled || manager != manager_) {
            stalled_ = {};
            previous_ = {};
            manager_ = manager;
        }
        if (!enabled) return false;
        const unsigned players = second_enabled() ? 2u : 1u;
        // A busy animation can leave the action unchanged even though the
        // original function already serviced its cursor. Both direct service
        // and action changes suppress recovery for that player only.
        for (unsigned player = 0; player < players; ++player)
            if (before[player] != after[player]) serviced |= 1u << player;
        bool recover = false;
        for (unsigned player = 0; player < 2; ++player) {
            const auto& action = after[player];
            if (player >= players || !action.type || (serviced & (1u << player))) stalled_[player] = 0;
            else {
                if (action.type != previous_[player].type || action.command != previous_[player].command)
                    stalled_[player] = 0;
                if (stalled_[player] < 30) ++stalled_[player];
                recover = recover || stalled_[player] >= 30;
            }
        }
        previous_ = after;
        if (!recover) return false;
        if (!(serviced & 1)) advance(0);
        // Match 82456D60: P1's update can change the guest two-player flag.
        if (second_enabled() && !(serviced & 2)) advance(1);
        return true;
    }
private:
    uint32_t manager_ = 0;
    NuiMenuActions previous_{};
    std::array<uint32_t, 2> stalled_{};
};
}
