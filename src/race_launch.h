#pragma once
#include <array>
#include <cstdint>

namespace sfr {
template<class Memory, class Action>
void with_active_launch_phase(Memory& memory, uint32_t owner, uint32_t current_record,
                              bool reset_countdown, Action action) {
    // State 6 writes waiting phase 1 and state 8 writes exit phase 3. Either
    // can run after a state-7 peer in the owner's twelve-record loop. The
    // owner then pauses that peer's launch timer while its motion continues.
    const uint32_t countdown=memory.template load<uint32_t>(uint64_t(owner)+3832);
    std::array<uint32_t,12> riders{};
    for(unsigned i=0;i<riders.size();++i) {
        const uint64_t record=uint64_t(owner)+400+i*224;
        if(record!=current_record && memory.template load<uint32_t>(record+4)==owner &&
           memory.template load<uint32_t>(record+84)==7)
            riders[i]=memory.template load<uint32_t>(record);
    }
    action();
    for(unsigned i=0;i<riders.size();++i) {
        const uint64_t record=uint64_t(owner)+400+i*224;
        if(riders[i] && memory.template load<uint32_t>(record)==riders[i] &&
           memory.template load<uint32_t>(record+4)==owner &&
           memory.template load<uint32_t>(record+84)==7) {
            // Only the scoped reset callback replaces the countdown. Ordinary
            // waiting/exit updates keep all original writes except the phase.
            if(reset_countdown)memory.template store<uint32_t>(uint64_t(owner)+3832,countdown);
            memory.template store<uint8_t>(uint64_t(owner)+3904,2);
            break;
        }
    }
}

// Forgotten Tomb's launch owner shares countdowns among twelve rider records.
// 8238D108 resets them on every exit update, even while waiting for that rider
// to land. Preserve the countdown belonging to an already waiting/launched
// peer, while letting the original exit perform all of its other actions.
template<class Memory, class Exit>
void with_launch_peer_countdowns(Memory& memory, uint32_t exiting_record, Exit exit) {
    const uint32_t owner=memory.template load<uint32_t>(uint64_t(exiting_record)+4);
    struct Peer { uint32_t rider{}, state{}; };
    std::array<Peer,12> peers{};
    const uint32_t wait=memory.template load<uint32_t>(uint64_t(owner)+3824);
    const uint32_t launch=memory.template load<uint32_t>(uint64_t(owner)+3832);
    for (unsigned i=0;i<peers.size();++i) {
        const uint64_t record=uint64_t(owner)+400+i*224;
        if (record==exiting_record || memory.template load<uint32_t>(record+4)!=owner) continue;
        const uint32_t state=memory.template load<uint32_t>(record+84);
        if (state==6 || state==7) peers[i]={memory.template load<uint32_t>(record),state};
    }

    exit();

    bool preserve_wait=false, preserve_launch=false;
    for (unsigned i=0;i<peers.size();++i) {
        const auto& peer=peers[i];
        if (!peer.rider) continue;
        const uint64_t record=uint64_t(owner)+400+i*224;
        // Do not carry an old cohort's timer into a removed, reused or advanced
        // record if any nested original cleanup changed its ownership/state.
        if (memory.template load<uint32_t>(record)!=peer.rider ||
            memory.template load<uint32_t>(record+4)!=owner ||
            memory.template load<uint32_t>(record+84)!=peer.state) continue;
        preserve_wait |= peer.state==6;
        preserve_launch |= peer.state==7;
    }
    if (preserve_wait) memory.template store<uint32_t>(uint64_t(owner)+3824,wait);
    if (preserve_launch) memory.template store<uint32_t>(uint64_t(owner)+3832,launch);
}
}
