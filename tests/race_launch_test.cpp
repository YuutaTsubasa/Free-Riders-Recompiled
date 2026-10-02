#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "race_launch.h"

struct Memory {
    std::array<unsigned char, 8192> bytes{};
    template<class T> T load(uint64_t p) const { T v; std::memcpy(&v, bytes.data()+p, sizeof(v)); return v; }
    template<class T> void store(uint64_t p, T v) { std::memcpy(bytes.data()+p, &v, sizeof(v)); }
};
constexpr uint32_t owner=128, rider=5000;
constexpr uint32_t record(unsigned slot) { return owner+400+224*slot; }
void require(bool ok, const char* why) { if (!ok) { std::cerr<<why<<'\n'; std::exit(1); } }
void admit(Memory& m, unsigned slot, uint32_t state) {
    m.store<uint32_t>(record(slot), rider+slot*4);
    m.store<uint32_t>(record(slot)+4, owner);
    m.store<uint32_t>(record(slot)+84, state);
}
Memory fixture(unsigned exit_slot=11) {
    Memory m;
    admit(m,exit_slot,0);
    m.store<uint32_t>(owner+3824,99);
    m.store<uint32_t>(owner+3832,37);
    return m;
}
// 8238D108 writes these before checking rider2208 bit8. If landing has
// not happened it retains the record, and repeats these resets next update.
void original_exit(Memory& m, unsigned exit_slot, bool landed=false) {
    m.store<uint32_t>(owner+3824,100);
    m.store<uint32_t>(owner+3832,70);
    m.store<uint32_t>(owner+3836,30);
    m.store<uint32_t>(record(exit_slot)+100,m.load<uint32_t>(record(exit_slot)+100)+1);
    if (landed) {
        m.store<uint32_t>(record(exit_slot),0);
        m.store<uint32_t>(record(exit_slot)+4,0);
        m.store<uint8_t>(owner+3904,0);
    }
}
void repeated_exit_does_not_starve_existing_peer() {
    for (auto state:{6u,7u}) for (auto exit_slot:{0u,11u}) {
        auto m=fixture(exit_slot);
        const unsigned peer_slot=exit_slot==0?11:0;
        admit(m,peer_slot,state);
        const uint32_t field=owner+(state==6?3824:3832);
        const uint32_t initial=m.load<uint32_t>(field);
        // The owner decrements after its twelve record callbacks. Exercise
        // both callback orders; an exit after the peer caused the captured stall.
        unsigned updates=0;
        while(m.load<uint32_t>(field)>0 && updates<120) {
            sfr::with_launch_peer_countdowns(m,record(exit_slot),[&]{original_exit(m,exit_slot);});
            m.store<uint32_t>(field,m.load<uint32_t>(field)-1);
            ++updates;
        }
        require(updates==initial,"existing peer countdown starved by repeated exit reset");
        require(m.load<uint32_t>(record(exit_slot)+100)==initial,"original exit actions were skipped");
        require(m.load<uint32_t>(record(exit_slot))!=0,"unlanded exit record was removed");
        require(m.load<uint32_t>(owner+3836)==30,"unrelated exit reset changed");
    }
}
void unrelated_and_new_cohorts_keep_original_reset() {
    for(auto state:{0u,1u,2u,3u,4u,5u,8u,9u}) {
        auto m=fixture(); admit(m,0,state); auto expected=m;
        original_exit(expected,11);
        sfr::with_launch_peer_countdowns(m,record(11),[&]{original_exit(m,11);});
        require(m.bytes==expected.bytes,"nonwaiting cohort changed original behavior");
    }
    auto m=fixture(); auto expected=m; original_exit(expected,11,true);
    sfr::with_launch_peer_countdowns(m,record(11),[&]{original_exit(m,11,true);});
    require(m.bytes==expected.bytes,"sole rider cleanup changed");
}
void protects_only_owned_counter_and_keeps_cleanup() {
    for (auto state:{6u,7u}) {
        auto m=fixture(); admit(m,0,state);
        sfr::with_launch_peer_countdowns(m,record(11),[&]{original_exit(m,11,true);});
        require(m.load<uint32_t>(owner+3824)==(state==6?99u:100u),"wrong wait countdown preserved");
        require(m.load<uint32_t>(owner+3832)==(state==7?37u:70u),"wrong launch countdown preserved");
        require(m.load<uint32_t>(record(11))==0 && m.load<uint8_t>(owner+3904)==0,"landing cleanup suppressed");
    }
    auto m=fixture(); admit(m,0,6); admit(m,1,7);
    m.store<uint32_t>(owner+3824,0); m.store<uint32_t>(owner+3832,0);
    sfr::with_launch_peer_countdowns(m,record(11),[&]{original_exit(m,11);});
    require(m.load<uint32_t>(owner+3824)==0 && m.load<uint32_t>(owner+3832)==0,"completed countdown restarted before transition");
}
void removed_or_reused_peer_does_not_retain_old_countdown() {
    for (unsigned variant=0;variant<5;++variant) {
        auto m=fixture(); admit(m,0,6);
        sfr::with_launch_peer_countdowns(m,record(11),[&]{
            original_exit(m,11);
            if(variant==0) m.store<uint32_t>(record(0),0);
            if(variant==1) m.store<uint32_t>(record(0),6000);
            if(variant==2) m.store<uint32_t>(record(0)+4,7000);
            if(variant==3) m.store<uint32_t>(record(0)+84,1);
            if(variant==4) m.store<uint32_t>(record(0)+84,7);
        });
        require(m.load<uint32_t>(owner+3824)==100,"departed peer retained old countdown");
    }
    auto m=fixture();
    sfr::with_launch_peer_countdowns(m,record(11),[&]{original_exit(m,11); admit(m,0,6);});
    require(m.load<uint32_t>(owner+3824)==100,"new admission inherited old countdown");
}
void waiting_peer_cannot_pause_active_launch() {
    for (auto active_slot:{0u,11u}) {
        const auto waiting_slot=11-active_slot;
        auto m=fixture(waiting_slot);admit(m,active_slot,7);
        m.store<uint8_t>(owner+3904,2);
        unsigned callbacks=0;
        for(unsigned update=0;update<37;++update) {
            sfr::with_active_launch_phase(m,owner,record(waiting_slot),false,[&] {
                ++callbacks;
                m.store<uint8_t>(owner+3904,1);
                m.store<uint32_t>(record(waiting_slot)+88,73);
            });
            // The original owner decrements only the counter of its final phase.
            if(m.load<uint8_t>(owner+3904)==2)
                m.store<uint32_t>(owner+3832,m.load<uint32_t>(owner+3832)-1);
        }
        require(m.load<uint32_t>(owner+3832)==0,"waiting peer paused active launch countdown");
        require(callbacks==37&&m.load<uint32_t>(record(waiting_slot)+88)==73,
                "waiting peer's original callback was skipped");
        require(m.load<uint32_t>(owner+3824)==99,"phase arbitration changed the waiting countdown");
    }
}
void reset_and_exit_preserve_only_surviving_launches() {
    for(bool reset:{false,true}) {
        auto m=fixture();admit(m,0,7);
        sfr::with_active_launch_phase(m,owner,record(11),reset,[&] {
            m.store<uint8_t>(owner+3904,reset?0:3);
            if(reset)m.store<uint32_t>(owner+3832,70);
            m.store<uint32_t>(record(11),0);
        });
        require(m.load<uint8_t>(owner+3904)==2&&m.load<uint32_t>(owner+3832)==37,
                "peer exit/reset interrupted a surviving launch");
        require(m.load<uint32_t>(record(11))==0,"peer exit cleanup suppressed");
    }
    for(unsigned change=0;change<4;++change) {
        auto m=fixture();admit(m,0,7);
        sfr::with_active_launch_phase(m,owner,record(11),true,[&] {
            m.store<uint8_t>(owner+3904,3);m.store<uint32_t>(owner+3832,70);
            if(change==0)m.store<uint32_t>(record(0),0);
            if(change==1)m.store<uint32_t>(record(0),6000);
            if(change==2)m.store<uint32_t>(record(0)+4,7000);
            if(change==3)m.store<uint32_t>(record(0)+84,8);
        });
        require(m.load<uint8_t>(owner+3904)==3&&m.load<uint32_t>(owner+3832)==70,
                "departed launch retained old phase/countdown");
    }
    for(auto state:{0u,4u,5u,6u,8u}) {
        auto m=fixture();admit(m,0,state);auto expected=m;
        auto original=[](Memory& x){x.store<uint8_t>(owner+3904,1);x.store<uint32_t>(owner+3832,70);};
        original(expected);
        sfr::with_active_launch_phase(m,owner,record(11),true,[&]{original(m);});
        require(m.bytes==expected.bytes,"nonlaunching peers changed original callback");
    }
    auto m=fixture();admit(m,11,7);
    sfr::with_active_launch_phase(m,owner,record(11),true,[&] {
        m.store<uint8_t>(owner+3904,3);m.store<uint32_t>(owner+3832,70);admit(m,0,7);
    });
    require(m.load<uint8_t>(owner+3904)==3&&m.load<uint32_t>(owner+3832)==70,
            "current or newly admitted rider inherited an older peer phase");
}
void nested_landing_reset_keeps_outer_countdown() {
    auto m=fixture();admit(m,0,7);
    sfr::with_launch_peer_countdowns(m,record(11),[&] {
        // D108 resets the countdown before its conditional C868 call.
        original_exit(m,11,true);
        sfr::with_active_launch_phase(m,owner,0,true,[&] {
            m.store<uint8_t>(owner+3904,0);
            m.store<uint32_t>(owner+3832,70);
        });
    });
    require(m.load<uint32_t>(owner+3832)==37&&m.load<uint8_t>(owner+3904)==2,
            "nested landing reset lost the outer peer countdown or launch phase");
    require(m.load<uint32_t>(record(11))==0,"nested reset undid original landing cleanup");
}
int main() {
    waiting_peer_cannot_pause_active_launch();
    reset_and_exit_preserve_only_surviving_launches();
    nested_landing_reset_keeps_outer_countdown();
    repeated_exit_does_not_starve_existing_peer();
    unrelated_and_new_cohorts_keep_original_reset();
    protects_only_owned_counter_and_keeps_cleanup();
    removed_or_reused_peer_does_not_retain_old_countdown();
    std::cout<<"race launch tests passed\n";
}
