#pragma once
#include "guest_memory.h"
#include "nui_skeleton.h"
#include <atomic>
#include <optional>

namespace sfr {
// The game's logical players are not necessarily skeleton slots 0 and 1.
// 82492470 looks up the cursor's player through [83E52F88]+120+4*player,
// then follows body+768 to NUI_SKELETON_DATA. 8245B5F8 reads its tracking
// ID at +4. Follow that same binding without moving slots or identities.
class NuiPlayerRouting {
public:
    // Called by the menu thread. A new active page owns a fresh cursor, but
    // the sensor thread may still be holding the previous confirmation pose.
    // Publish a request rather than mutating a skeleton from this thread.
    // Hidden/activating guest cursors keep their initial raise pending until
    // the menu consumes it; page existence alone does not mean it updates.
    void observe_menu_page(uint32_t player, uint32_t page, uint32_t kind, bool cursor_pending = false) {
        if (player >= 2) return;
        const uint32_t bit=1u << player;
        if (page && cursor_pending) cursor_pending_.fetch_or(bit,std::memory_order_relaxed);
        else cursor_pending_.fetch_and(~bit,std::memory_order_relaxed);
        auto& previous = pages_[player];
        if (!page) { previous.active = false; return; }
        if (previous.seen && (!previous.active || previous.address != page || previous.kind != kind))
            rearm_.fetch_or(1u << player, std::memory_order_relaxed);
        previous = {page, kind, true, true};
    }
    bool reversed() const { return reversed_; }
    bool slot_present(uint32_t slot, bool second_present) const {
        return second_present || slot == (reversed_ ? 1u : 0u);
    }
    // Camera input replaces logical P1's joints, just as P1's pad gestures
    // follow that player's binding. Keep each slot's tracking ID/enrollment.
    void write_slots(GuestMemory& memory, uint32_t frame,
                     const NuiSkeletonEmulation& slot0, const NuiSkeletonEmulation& slot1,
                     bool second_present,
                     const std::array<std::array<float,3>,nui_joint_count>* camera = nullptr) const {
        for(uint32_t slot=0;slot<2;++slot) {
            if(!slot_present(slot,second_present)) continue;
            const auto& skeleton=slot==0?slot0:slot1;
            if(camera && slot==(reversed_?1u:0u)) skeleton.write_joints(memory,frame,slot,slot+1,*camera);
            else skeleton.write_slot(memory,frame,slot,slot+1);
        }
    }
    void update(const GuestMemory& memory, NuiSkeletonEmulation& slot0, NuiSkeletonEmulation& slot1,
                const GamepadState& first, const std::optional<GamepadState>& second,
                bool racing, bool camera, bool controller_handoff = false) {
        if (const auto order = guest_order(memory, second.has_value())) reversed_ = *order;
        // An incomplete binding during a page/Loading transition does not
        // reverse the last known assignment. Race body input stays indexed
        // by logical player; these are only that player's skeleton gestures.
        auto& first_skeleton = reversed_ ? slot1 : slot0;
        auto& second_skeleton = reversed_ ? slot0 : slot1;
        if(controller_handoff && !racing) first_skeleton.rearm_menu();
        // A transient P2 disconnect must not consume the page transition.
        // Camera discards P1's menu requests only. P2's controller still
        // needs its own Gear rearm/acknowledgement while P1 uses camera.
        const auto rearm = (racing || camera || second)
            ? rearm_.exchange(0, std::memory_order_relaxed) : 0u;
        if (!racing && second) {
            if (!camera && (rearm & 1)) first_skeleton.rearm_menu();
            if (rearm & 2) second_skeleton.rearm_menu();
        }
        if (racing) cursor_pending_.store(0,std::memory_order_relaxed);
        else if (camera) cursor_pending_.fetch_and(~1u,std::memory_order_relaxed);
        const uint32_t pending=(!racing && second)
            ? cursor_pending_.load(std::memory_order_relaxed) : 0u;
        first_skeleton.update(first, racing, second.has_value(),(pending & 1)!=0);
        if (second) second_skeleton.update(*second, racing, true,(pending & 2)!=0);
    }
private:
    static std::optional<bool> guest_order(const GuestMemory& memory, bool second_present) {
        constexpr uint32_t players = 0x83E52F88;
        if (!memory.readable(players, 4)) return std::nullopt;
        const uint32_t manager = memory.load<uint32_t>(players);
        if (!manager || !memory.readable(uint64_t(manager) + 120, 8)) return std::nullopt;
        uint32_t ids[2]{};
        for (uint32_t player = 0; player < (second_present ? 2u : 1u); ++player) {
            const uint32_t body = memory.load<uint32_t>(uint64_t(manager) + 120 + 4 * player);
            if (!body || !memory.readable(uint64_t(body) + 768, 4)) return std::nullopt;
            const uint32_t data = memory.load<uint32_t>(uint64_t(body) + 768);
            if (!data || !memory.readable(data, 8) || memory.load<uint32_t>(data) != nui_tracked)
                return std::nullopt;
            ids[player] = memory.load<uint32_t>(uint64_t(data) + 4);
        }
        if (!second_present && (ids[0] == 1 || ids[0] == 2)) return ids[0] == 2;
        if (ids[0] == 1 && ids[1] == 2) return false;
        if (ids[0] == 2 && ids[1] == 1) return true;
        return std::nullopt;
    }
    bool reversed_ = false;
    struct Page { uint32_t address = 0, kind = 0; bool seen = false, active = false; };
    Page pages_[2]; // menu thread only
    std::atomic<uint32_t> rearm_{0};
    std::atomic<uint32_t> cursor_pending_{0}; // logical players, published by menu thread
};
}
