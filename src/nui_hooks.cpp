#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "nui_skeleton.h"
#include "nui_speech.h"
#include "local_profile.h"
#include "touch_controls.h"
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <string_view>

// Kinect emulation at the NUI API the title links statically. Without a
// sensor the original NuiInitialize fails and the title never leaves its
// start screen, which only answers Kinect gestures. These replace the five
// entry points the title's Kinect manager uses (identified from its calls:
// 822130D0 initializes and enables tracking with its Event_NuiGetSkeleton,
// the 824395E8 thread waits on that event and fetches frames, 824387E8
// shuts down). Other library functions keep their original bodies and fail
// as before, since the library's own state stays uninitialized.

namespace {
sfr::NuiSkeletonEmulation skeleton;
// A second Kinect player, driven by the second pad. The frame carries six
// skeleton slots and the title reads them all, so a player appears simply by
// filling another one; it is identified separately (tracking id 2).
sfr::NuiSkeletonEmulation second_skeleton;
bool second_present = false;
sfr::NuiPadEdges edges;
uint16_t pressed=0;  // buttons newly pressed at the last input update
uint32_t frame_number = 0;
const auto started = std::chrono::steady_clock::now();
}

namespace {
// The title's frame step in seconds ([[83E516A0]+24]+40), which the menus add
// to a delayed action's elapsed time.
float frame_step(sfr::GuestMemory& memory) {
    const uint32_t clock=memory.load<uint32_t>(0x83E516A0);
    if(!clock) return 0.0f;
    return std::bit_cast<float>(memory.load<uint32_t>(memory.load<uint32_t>(clock+24)+40));
}

// Calls a guest function with up to four arguments and returns r3.
uint32_t call_guest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                    uint32_t r3, uint32_t r4=0, uint32_t r5=0, uint32_t r6=0) {
    PPCContext saved=ctx;
    ctx.r3.u64=r3; ctx.r4.u64=r4; ctx.r5.u64=r5; ctx.r6.u64=r6;
    function(ctx,base);
    const uint32_t result=ctx.r3.u32;
    ctx=saved;
    return result;
}

// The menu buttons of every page of the menu manager (+80..+84: 8-byte
// entries, page first; page +300..+304: 8-byte entries, button first).
template<class F> void for_each_menu_button(sfr::GuestMemory& memory, uint32_t manager, F visit) {
    const uint32_t pages=memory.load<uint32_t>(manager+80), pages_end=memory.load<uint32_t>(manager+84);
    for(uint32_t entry=pages; entry<pages_end && entry-pages<8*64; entry+=8) {
        const uint32_t page=memory.load<uint32_t>(entry);
        if(!page) continue;
        const uint32_t buttons=memory.load<uint32_t>(page+300), buttons_end=memory.load<uint32_t>(page+304);
        for(uint32_t slot=buttons; slot<buttons_end && slot-buttons<8*128; slot+=8)
            if(const uint32_t button=memory.load<uint32_t>(slot)) if(!visit(button)) return;
    }
}
uint32_t menu_manager=0;        // last manager whose update ran
uint64_t menu_manager_frame=0;  // input update count at that time
uint64_t input_frames=0;
}


// NuiInitialize(flags, ?)
SFR_HOOK(sub_8276FD88) {
    sfr::enter_function(ctx,"sub_8276FD88",0x8276FD88);
    std::cerr << "NUI_INITIALIZE flags=0x" << std::hex << ctx.r3.u32 << " argument=0x" << ctx.r4.u32
              << std::dec << " result=0 backend=emulated-skeleton\n";
    ctx.r3.u64=0;
}

// NuiShutdown()
SFR_HOOK(sub_8276DF30) {
    sfr::enter_function(ctx,"sub_8276DF30",0x8276DF30);
    sfr::stop_nui_skeleton_events();
    ctx.r3.u64=0;
}

// NuiSkeletonTrackingEnable(next frame event, flags): the event is signaled
// at the sensor's 30 Hz.
SFR_HOOK(sub_82770668) {
    sfr::enter_function(ctx,"sub_82770668",0x82770668);
    std::cerr << "NUI_SKELETON_TRACKING_ENABLE event=0x" << std::hex << ctx.r3.u32 << " flags=0x" << ctx.r4.u32
              << std::dec << '\n';
    if(ctx.r3.u32) sfr::start_nui_skeleton_events(ctx.r3.u32);
    ctx.r3.u64=0;
}

// NuiSkeletonTrackingDisable()
SFR_HOOK(sub_8276FEE0) {
    sfr::enter_function(ctx,"sub_8276FEE0",0x8276FEE0);
    sfr::stop_nui_skeleton_events();
    ctx.r3.u64=0;
}

// NuiSkeletonGetNextFrame(timeout ms, frame): one emulated player.
SFR_HOOK(sub_827707B0) {
    sfr::enter_function(ctx,"sub_827707B0",0x827707B0);
    const uint32_t frame=ctx.r4.u32;
    if(!frame) { ctx.r3.u64=0x80004003u; return; }  // E_POINTER, as the original
    // [83E52F8C] is the race flag: the hands leave the menu cursor pose.
    const bool racing=sfr::active_memory->load<uint32_t>(0x83E52F8C) != 0;
    sfr::set_touch_racing(racing);
    skeleton.update(sfr::nui_gamepad(), racing);
    const auto second=sfr::nui_second_gamepad();
    if(second) second_skeleton.update(*second, racing);
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started);
    auto& memory=*sfr::active_memory;
    sfr::NuiSkeletonEmulation::write_header(memory,frame,++frame_number,uint64_t(elapsed.count()));
    skeleton.write_slot(memory,frame,0,1);
    if(second) second_skeleton.write_slot(memory,frame,1,2);
    if(second.has_value()!=second_present) {
        second_present=second.has_value();
        std::cerr << "NUI_SECOND_PLAYER present=" << second_present << '\n';
    }
    if(frame_number%300==1) {
        const auto hand=skeleton.hand(true);
        std::cerr << "NUI_SKELETON_FRAME number=" << frame_number << " right_hand=" << hand[0] << ',' << hand[1]
                  << ',' << hand[2] << '\n';
    }
    ctx.r3.u64=0;
}

// NuiIdentityIdentify(tracking id, flags, callback, context): the title
// identifies each new skeleton before it joins as a player (82437E48). The
// emulated player is the local profile (enrollment 0) when one is signed in,
// so the title plays as that profile and keeps its records; otherwise an
// unenrolled guest. The completion message (id 1, result S_OK, enrollment)
// is delivered to the callback at once and the call reports the pending
// asynchronous operation it started.
SFR_HOOK(sub_82764620) {
    sfr::enter_function(ctx,"sub_82764620",0x82764620);
    const uint32_t tracking_id=ctx.r3.u32, flags=ctx.r4.u32, callback=ctx.r5.u32, context=ctx.r6.u32;
    if(flags) { ctx.r3.u64=0x80070057u; return; }  // E_INVALIDARG, as the original
    auto& memory=*sfr::active_memory;
    const uint32_t message=(ctx.r1.u32-0x400)&~15u;  // below the caller's frame
    memory.check_write(message,32);
    for(uint32_t offset=0;offset<32;offset+=4) memory.store<uint32_t>(uint64_t(message)+offset,0);
    memory.store<uint32_t>(message,1);                         // identity operation complete
    memory.store<uint32_t>(uint64_t(message)+4,tracking_id);
    // Only the first player is the signed-in profile; a second one joins as
    // an unenrolled guest, as a friend standing beside the sensor would.
    const bool profile=tracking_id<=1 && sfr::profile_for(0)!=nullptr;
    const uint32_t enrollment=profile?0u:sfr::NuiSkeletonEmulation::guest;
    memory.store<uint32_t>(uint64_t(message)+12,enrollment);
    (tracking_id>=2?second_skeleton:skeleton).identify(enrollment);
    std::cerr << "NUI_IDENTITY_IDENTIFY tracking_id=" << tracking_id << " callback=0x" << std::hex << callback
              << " context=0x" << context << std::dec << " result=" << (profile?"profile":"guest") << '\n';
    if(callback) {
        PPCContext saved=ctx;
        ctx.r3.u64=context;
        ctx.r4.u64=message;
        ctx.r1.u64=message-0x100;
        sfr::call_indirect(ctx,base,callback);
        ctx=saved;
    }
    ctx.r3.u64=0x8000000Au;  // E_PENDING
}

PPC_FUNC_IMPL(__imp__sub_82494658);

// Input update (once per frame, input object = [83E52FB8]): after the
// original has published the recognizer's word, a newly pressed pad button
// is heard as a voice command at +5440 with high confidence (+5444 score,
// +5448 level 2). [83E52F8C] is nonzero during a race. The buttons pressed
// since the previous update stay available to the hand-only dialogs below
// until the next update.
SFR_HOOK(sub_82494658) {
    sfr::enter_function(ctx,"sub_82494658",0x82494658);
    const uint32_t input=ctx.r3.u32;
    auto& memory=*sfr::active_memory;
    __imp__sub_82494658(ctx,base);
    const bool racing=memory.load<uint32_t>(0x83E52F8C)!=0;
    pressed=edges.update(sfr::nui_gamepad().buttons);
    ++input_frames;
    uint16_t spoken=pressed;
    // B of a menu with a back button is that button's (824578F0 below).
    if((pressed & sfr::gamepad_button::b) && menu_manager && input_frames-menu_manager_frame<=2) {
        bool back=false;
        for_each_menu_button(memory,menu_manager,[&](uint32_t b) { back=memory.load<uint32_t>(b+288)==31; return !back; });
        if(back) spoken&=~sfr::gamepad_button::b;
    }
    // SFR_SAY="word@present,..." speaks any word of the title's vocabulary at
    // the given present, to find which one a page answers to (the words live
    // in the image around 0x821A84D8: next, restart, replay, mainmenu,
    // courseslc, ruleslc, gearslc, charaslc, playernumslc, missionslc).
    static const auto script=[]{
        std::vector<std::pair<uint32_t,std::string>> said;
        const char* text=std::getenv("SFR_SAY");
        for(std::string_view rest=text?text:""; !rest.empty();) {
            const auto comma=rest.find(',');
            const auto entry=rest.substr(0,comma);
            const auto at=entry.find('@');
            if(at!=std::string_view::npos)
                said.emplace_back(uint32_t(std::strtoul(std::string(entry.substr(at+1)).c_str(),nullptr,10)),
                                  std::string(entry.substr(0,at)));
            rest=comma==std::string_view::npos ? std::string_view{} : rest.substr(comma+1);
        }
        return said;
    }();
    static size_t said_index=0;
    if(said_index<script.size() && sfr::present_count>=script[said_index].first) {
        const auto& entry=script[said_index++];
        memory.check_write(uint64_t(input)+5440,12);
        memory.store<uint32_t>(uint64_t(input)+5440,sfr::NuiSpeechEmulation::say(memory,entry.second));
        memory.store<uint32_t>(uint64_t(input)+5444,0x3F800000u);
        memory.store<uint32_t>(uint64_t(input)+5448,2);
        std::cerr << "NUI_SAID word=" << entry.second << " present=" << sfr::present_count << '\n';
        return;
    }
    const auto word=sfr::NuiSpeechEmulation::hear(spoken,racing);
    if(word==sfr::NuiSpeechEmulation::Word::none) return;
    static bool written=false;
    if(!written) { sfr::NuiSpeechEmulation::write_words(memory); written=true; }
    memory.check_write(uint64_t(input)+5440,12);
    memory.store<uint32_t>(uint64_t(input)+5440,sfr::NuiSpeechEmulation::address(word));
    memory.store<uint32_t>(uint64_t(input)+5444,0x3F800000u);  // 1.0f
    memory.store<uint32_t>(uint64_t(input)+5448,2);
    const auto text=sfr::NuiSpeechEmulation::text(word);
    std::cerr << "NUI_SPEECH word=" << std::string(text.begin(),text.end()) << " racing=" << racing << '\n';
}

PPC_FUNC_IMPL(__imp__sub_823EF348);

// Hand-pointer dialog choice (dialog, hand): the slot 0..2 whose button the
// hand held long enough, or 3 for none. Used by the title's Omochao prompt
// (8244F880) and two other dialogs; +124 is the layout, 4 for the two
// buttons in slots 0 (yes) and 1 (no), 3 for a single button in slot 2. The
// original only answers the hand, so a pressed A or B chooses directly, with
// the confirm (16) or cancel (8) sound the original plays through 8222FA18.
SFR_HOOK(sub_823EF348) {
    sfr::enter_function(ctx,"sub_823EF348",0x823EF348);
    auto& memory=*sfr::active_memory;
    const uint32_t dialog=ctx.r3.u32;
    const int slot=ctx.r4.u32 ? sfr::dialog_choice(pressed,memory.load<uint32_t>(uint64_t(dialog)+124)) : -1;
    if(slot<0) { __imp__sub_823EF348(ctx,base); return; }
    pressed=0;  // one choice per press
    PPCContext saved=ctx;
    ctx.r3.u64=memory.load<uint32_t>(0x83E52E18);
    ctx.r4.u64=slot==1 ? 8 : 16;
    ctx.r5.u64=1;
    sub_8222FA18(ctx,base);
    ctx=saved;
    std::cerr << "NUI_DIALOG_CHOICE dialog=0x" << std::hex << dialog << std::dec << " slot=" << slot << '\n';
    ctx.r3.u64=uint64_t(slot);
}

PPC_FUNC_IMPL(__imp__sub_8246A6D0);

// Ring menu step for one player (the object's first word is the player,
// 1-based, 0 for the first): the hand swipe as a signed number of items.
// Its callers (823F3148, 82454360, 824543D0) add both players' steps, so a
// newly pressed D-pad left or right of the first player turns the ring one
// item when the hand did not, once per press.
SFR_HOOK(sub_8246A6D0) {
    sfr::enter_function(ctx,"sub_8246A6D0",0x8246A6D0);
    const uint32_t player=sfr::active_memory->load<uint32_t>(ctx.r3.u32);
    __imp__sub_8246A6D0(ctx,base);
    const int step=sfr::ring_step(pressed);
    if(player>1 || ctx.r3.u32 || !step) return;
    pressed&=~(sfr::gamepad_button::dpad_left|sfr::gamepad_button::dpad_right);
    ctx.r3.s64=step;
}

PPC_FUNC_IMPL(__imp__sub_824578F0);

// Menu update (this = manager+36). Buttons carry a type at +288 and flags at
// +492; hands select them by holding. Without a hand, B takes the page's
// back button (type 31: 8245B130 asks whether it may leave, 824603B8 leaves)
// and Y an enabled (+492 bit 0x01000000) shortcut button (types 33..35, the
// parts shop: menu commands 15, 16 and 49 through 82454848, with sound 18).
SFR_HOOK(sub_824578F0) {
    sfr::enter_function(ctx,"sub_824578F0",0x824578F0);
    auto& memory=*sfr::active_memory;
    const uint32_t manager=ctx.r3.u32-36;
    menu_manager=manager;
    menu_manager_frame=input_frames;
    // SFR_MENU_DUMP=1 reports each menu page: its buttons as type/flags/kind/state
    // and the player's current page, whenever they change (a debugging aid).
    if(std::getenv("SFR_MENU_DUMP")) {
        static std::string last;
        std::string line;
        for_each_menu_button(memory,manager,[&](uint32_t b) {
            char text[64];
            std::snprintf(text,sizeof text," %u/%08x/%u/%x",memory.load<uint32_t>(b+288),memory.load<uint32_t>(b+492),
                          memory.load<uint32_t>(b+312),memory.load<uint32_t>(b+308));
            line+=text;
            return true;
        });
        // The player's current page (82457348(manager, player)) and its buttons.
        const uint32_t page=call_guest(ctx,base,sub_82457348,manager,0);
        line+=" | page";
        if(page) {
            char head[128];
            std::snprintf(head,sizeof head," @%08x vt=%08x m348=%u m352=%u m1848=%u s128=%08x s136=%08x",page,
                          memory.load<uint32_t>(page),memory.load<uint32_t>(page+348),memory.load<uint32_t>(page+352),
                          memory.load<uint32_t>(page+1848),memory.load<uint32_t>(page+128),memory.load<uint32_t>(page+136));
            line+=head;
            const uint32_t b0=memory.load<uint32_t>(page+300), b1=memory.load<uint32_t>(page+304);
            for(uint32_t slot=b0; slot<b1 && slot-b0<8*64; slot+=8) {
                const uint32_t b=memory.load<uint32_t>(slot);
                if(!b) continue;
                char text[64];
                std::snprintf(text,sizeof text," %u/%08x/%u/%x",memory.load<uint32_t>(b+288),memory.load<uint32_t>(b+492),
                              memory.load<uint32_t>(b+312),memory.load<uint32_t>(b+308));
                line+=text;
            }
        }
        if(line!=last) { last=line; std::cerr << "MENU_BUTTONS manager=0x" << std::hex << manager << std::dec << line << '\n'; }
    }
    namespace button=sfr::gamepad_button;
    if(pressed & button::b) {
        uint32_t back=0;
        for_each_menu_button(memory,manager,[&](uint32_t b) {
            if(memory.load<uint32_t>(b+288)==31) { back=b; return false; }
            return true;
        });
        if(back) {
            pressed&=~button::b;
            const uint32_t page=memory.load<uint32_t>(back+280);
            if(call_guest(ctx,base,sub_8245B130,page)&0xFF) {
                const uint32_t side=(memory.load<uint32_t>(memory.load<uint32_t>(back+284)+320)>>4)&1;
                call_guest(ctx,base,sub_824603B8,page,side);
                std::cerr << "NUI_MENU_BACK button=0x" << std::hex << back << std::dec << '\n';
            }
        }
    }
    // A page changes through a delayed action: the menu update (82456700)
    // keeps its type at manager+488, its delay at +492, its elapsed time at
    // +496 and its command at +500, and adds the title's frame step
    // ([[83E516A0]+24]+40) to the elapsed time until it passes the delay. On
    // some pages (the gear parts) the title stops calling that update while
    // an action is queued, so the elapsed time never moves and the page never
    // leaves, whatever the player presses; calling the update by hand
    // advances it (elapsed 0 to 1 in the first measurement).
    //
    // While the elapsed time has not moved for half a second, the update runs
    // here, which elapses the action and fires its command through the
    // title's own path. SFR_MENU_STALL=0 leaves the page as it is.
    // SFR_MENU_STATE=1: the manager's state ([r3+4]) and the queued action, on
    // every change and every two seconds. The state dispatches as state - 2:
    // 2 and 3 open the pages, 4 runs them (it is where the page update
    // 82456D60 -> 82456700 is called from), 5 builds the page change's fade
    // into +436 and 6 waits for it to reach [[r3+436]+20] == 3.
    static const bool state_trace=[]{ const char* t=std::getenv("SFR_MENU_STATE"); return t && *t!='0'; }();
    // SFR_WATCH_SLOT=<0|1>: watch that player slot's "pending request" word
    // ([r3+1828+88*slot+20], which 82452F48 waits on), so each change reports
    // the function entered right after it.
    static const int watch_slot=[]{ const char* t=std::getenv("SFR_WATCH_SLOT"); return t ? std::atoi(t) : -1; }();
    if(watch_slot>=0 && !sfr::watch_word.load(std::memory_order_relaxed))
        sfr::watch_word.store(ctx.r3.u32+1828+88*uint32_t(watch_slot)+20,std::memory_order_relaxed);
    static uint32_t last_state=~0u;
    const uint32_t state=memory.load<uint32_t>(ctx.r3.u32+4);
    if(state_trace && (input_frames%120==0 || state!=last_state))
    {
        last_state=state;
        const uint32_t waited=memory.load<uint32_t>(ctx.r3.u32+436);
        std::cerr << "NUI_MENU_STATE frame=" << input_frames << " state=" << state
                  << " action=" << memory.load<uint32_t>(manager+488)
                  << " elapsed=" << std::bit_cast<float>(memory.load<uint32_t>(manager+496))
                  << " waited=0x" << std::hex << waited << std::dec
                  << " waited20=" << (waited ? memory.load<uint32_t>(waited+20) : 0u);
        // State 5 asks [[83E5160C]]->vtable[4](100, 16) for the fade object
        // state 6 then waits on.
        const uint32_t fader=memory.load<uint32_t>(0x83E5160C);
        const uint32_t vtable=fader ? memory.load<uint32_t>(fader) : 0;
        std::cerr << std::hex << " fader=0x" << fader << " vtable=0x" << vtable << " method=0x"
                  << (vtable ? memory.load<uint32_t>(vtable+16) : 0u) << std::dec << '\n';
        // State 5 only builds that fade once both player slots (+1828 and
        // +1916, 88 bytes each) have latched their "settled" byte at +59 and
        // the queue at [[83E52FB8]+5128 .. +5132] is empty; any of the three
        // failing leaves the state as it is, for good.
        if(state==5) try {
            const uint32_t queue=memory.load<uint32_t>(0x83E52FB8);
            std::cerr << "NUI_MENU_LEAVE queue=0x" << std::hex << queue << std::dec;
            if(queue)
                std::cerr << " first=" << memory.load<uint32_t>(queue+5128)
                          << " last=" << memory.load<uint32_t>(queue+5132);
            for(uint32_t slot=0; slot<2; ++slot) {
                const uint32_t at=ctx.r3.u32+1828+88*slot;
                std::cerr << " | slot" << slot << " @0x" << std::hex << at << std::dec;
                for(uint32_t word=0; word<22; ++word)
                    std::cerr << ' ' << word*4 << '=' << memory.load<uint32_t>(at+word*4);
            }
            std::cerr << '\n';
        } catch(const std::exception& error) {
            std::cerr << "NUI_MENU_LEAVE unreadable: " << error.what() << '\n';
        }
    }
    const uint32_t action=memory.load<uint32_t>(manager+488);
    const uint32_t elapsed=memory.load<uint32_t>(manager+496);
    static uint32_t last_elapsed=0;
    static uint64_t stalled_frames=0;
    stalled_frames=(action && elapsed==last_elapsed) ? stalled_frames+1 : 0;
    last_elapsed=elapsed;
    static const bool run_stalled=[]{ const char* t=std::getenv("SFR_MENU_STALL"); return !t || *t!='0'; }();
    if(run_stalled && stalled_frames>=30) {
        if(stalled_frames==30)
            std::cerr << "NUI_MENU_UPDATE_RUN manager=0x" << std::hex << manager << std::dec << " type=" << action
                      << " delay=" << std::bit_cast<float>(memory.load<uint32_t>(manager+492))
                      << " command=" << memory.load<uint32_t>(manager+500) << '\n';
        call_guest(ctx,base,sub_82456700,manager,0);
    }
    if(pressed & button::y) {
        uint32_t command=0;
        for_each_menu_button(memory,manager,[&](uint32_t b) {
            if(memory.load<uint32_t>(b+492)&0x01000000) command=sfr::menu_shortcut_command(memory.load<uint32_t>(b+288));
            return command==0;
        });
        if(command) {
            pressed&=~button::y;
            call_guest(ctx,base,sub_82454848,manager,0,3,command);
            call_guest(ctx,base,sub_8222FA18,memory.load<uint32_t>(0x83E52E18),18,8);
            std::cerr << "NUI_MENU_SHORTCUT command=" << command << '\n';
        }
    }
    __imp__sub_824578F0(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_82439530);

// Kinect depth view update on the manager's worker thread: it takes the
// enabled flag (+7756) and the "DepthView" resource (+7736) without a lock,
// and the resource is created on the main thread (82438CC8). Until it is
// there the update has nothing to do; the original would read through the
// null pointer.
SFR_HOOK(sub_82439530) {
    sfr::enter_function(ctx,"sub_82439530",0x82439530);
    if(!sfr::active_memory->load<uint32_t>(uint64_t(ctx.r3.u32)+7736)) {
        static uint32_t skipped=0;
        if(skipped++<4) std::cerr << "NUI_DEPTH_VIEW_SKIPPED manager=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    __imp__sub_82439530(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_82463808);

// "A part is still moving": the menu update (82456700) drops every delayed
// action of every page while this reports one. On the gear parts page the
// page change is such a delayed action, and the report never clears, so the
// page never leaves. The original looks up the part object the scene list
// holds ([83E5312C], entries with +344 == 6 and +336 == 56) and answers from
// its two fields at +364 and +396.
//
// After a second of nothing else happening the answer becomes "no": the
// animation it waits for is the part flying onto the board, and whatever
// keeps it pending here, the page it blocks is the one the player asked for.
SFR_HOOK(sub_82463808) {
    sfr::enter_function(ctx,"sub_82463808",0x82463808);
    const uint32_t manager=ctx.r3.u32;
    __imp__sub_82463808(ctx,base);
    static uint32_t busy=0;
    if(!(ctx.r3.u32&0xFF)) { busy=0; return; }
    if(++busy<120) {
        // The pairs it answers from: the parts state ([manager+56]) keeps a part
        // and its animation at +364/+396 and +400/+432, and a running slide at
        // +392/+428.
        if(busy==1) {
            auto& parts=*sfr::active_memory;
            const uint32_t state=parts.load<uint32_t>(manager+56);
            std::cerr << "NUI_MENU_PART_BUSY manager=0x" << std::hex << manager << " state=0x" << state << std::dec;
            if(state)
                std::cerr << " f364=" << parts.load<uint32_t>(state+364)
                          << " f396=" << parts.load<uint32_t>(state+396)
                          << " f400=" << parts.load<uint32_t>(state+400)
                          << " f432=" << parts.load<uint32_t>(state+432)
                          << " f392=" << parts.load<uint32_t>(state+392)
                          << " f428=" << parts.load<uint32_t>(state+428);
            std::cerr << '\n';
        }
        return;
    }
    if(busy==120) std::cerr << "NUI_MENU_PART_RELEASED after=" << busy << " calls\n";
    ctx.r3.u64=0;
}

PPC_FUNC_IMPL(__imp__sub_824A3398);

// Pool allocation (pool, size, alignment, ?): the menus ask this for the fade
// object a page transition waits on ([[83E5160C]]->vtable[4](100, 16)), and on
// the gear parts page it comes back null, which is why that page never leaves.
// Reports the first failures with the pool's free list head, to tell an
// exhausted pool from a wrong argument.
SFR_HOOK(sub_824A3398) {
    sfr::enter_function(ctx,"sub_824A3398",0x824A3398);
    const uint32_t pool=ctx.r3.u32, size=ctx.r4.u32, alignment=ctx.r5.u32;
    __imp__sub_824A3398(ctx,base);
    if(ctx.r3.u32) return;
    static std::atomic<uint32_t> failures{0};
    if(failures++>=8) return;
    auto& memory=*sfr::active_memory;
    const uint32_t list=memory.load<uint32_t>(pool+36);
    std::cerr << "NUI_POOL_EMPTY pool=0x" << std::hex << pool << " size=" << std::dec << size
              << " alignment=" << alignment << " list=0x" << std::hex << list
              << " head=0x" << (list ? memory.load<uint32_t>(list) : 0u) << std::dec << '\n';
}

PPC_FUNC_IMPL(__imp__sub_8243D1B8);

// The page transition's fade, started only when the object the manager waits
// on exists: reporting its calls says whether that object was never created
// or created and then lost (see docs/pad-menus.md).
SFR_HOOK(sub_8243D1B8) {
    sfr::enter_function(ctx,"sub_8243D1B8",0x8243D1B8);
    static uint32_t calls=0;
    if(calls++<8)
        std::cerr << "NUI_MENU_FADE object=0x" << std::hex << ctx.r3.u32 << " a=" << std::dec << ctx.r4.u32
                  << " b=" << ctx.r5.u32 << " colour=0x" << std::hex << ctx.r6.u32 << std::dec << '\n';
    __imp__sub_8243D1B8(ctx,base);
}
