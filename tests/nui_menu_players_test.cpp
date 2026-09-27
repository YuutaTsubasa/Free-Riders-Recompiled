#include "nui_skeleton.h"
#include "guest_memory.h"
#include "nui_player_routing.h"
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void menu(sfr::NuiSkeletonEmulation& player, const sfr::GamepadState& pad = {}) {
    player.update(pad, false, true);
}
void run() {
    sfr::NuiSkeletonEmulation first, second;
    const auto resting_left = first.hand(false), resting_right = first.hand(true);
    first.identify(0);
    second.identify();
    for (unsigned frame = 0; frame < 30; ++frame) { menu(first); menu(second); }
    require(first.hand(true)[2] == 2.1f && second.hand(true)[2] == 2.1f,
            "both menu cursors must keep the forward activation pose without RB");
    const auto first_start = first.hand(true), second_start = second.hand(true);
    sfr::GamepadState move;
    move.thumb_rx = 32767;
    menu(first, move);
    menu(second);
    require(first.hand(true)[0] > first_start[0] && second.hand(true) == second_start,
            "first right stick must move only the first cursor");
    const auto first_moved = first.hand(true);
    move.thumb_rx = -32768;
    menu(second, move);
    require(second.hand(true)[0] < second_start[0] && first.hand(true) == first_moved,
            "second right stick must move only the second cursor");

    sfr::GamepadState left;
    left.thumb_lx = left.thumb_ly = 32767;
    left.buttons = sfr::gamepad_button::left_shoulder;
    for (unsigned frame = 0; frame < 60; ++frame) menu(first, left);
    require(first.hand(false) == resting_left && first.hand(true) == first_moved,
            "left stick and LB must not introduce a competing menu hand");
    move.thumb_rx = move.thumb_ry = -32768;
    for (unsigned frame = 0; frame < 60; ++frame) menu(first, move);
    require(first.hand(true)[2] == 2.1f,
            "low inward swipe must retain the active cursor depth");

    sfr::GuestMemory memory;
    constexpr uint32_t address = 0x10000;
    memory.map(address, 0x4000);
    sfr::NuiSkeletonEmulation::write_header(memory, address, 1, 0);
    first.write_slot(memory, address, 0, 1);
    second.write_slot(memory, address, 1, 2);
    const auto data = address + sfr::nui_skeleton_data_offset;
    const auto other = data + sfr::nui_skeleton_data_size;
    require(memory.load<uint32_t>(data + 4) == 1 && memory.load<uint32_t>(data + 12) == 0 &&
            memory.load<uint32_t>(other + 4) == 2 && memory.load<uint32_t>(other + 12) == 1,
            "menu changes must preserve each player's tracking ID and user index");

    first.update(left, true, true);
    require(first.hand(true) == resting_right && first.hand(false) == resting_left,
            "race entry must lower menu hands");
    move = {};
    move.thumb_ry = 32767;
    first.update(move, true, true);
    require(first.hand(true)[1] > 0.7f && first.hand(false)[1] > 0.7f,
            "racing must retain both-arm trick gestures");
    menu(first, move);
    require(first.hand(false) == resting_left && first.hand(true)[2] == 2.1f,
            "returning to menus must restore only the right cursor");
    move = {};
    move.buttons = sfr::gamepad_button::back;
    menu(first, move);
    require(first.hand(true) == resting_right && first.hand(false) == resting_left,
            "BACK must still lower both hands");
}
void menu_rearm_scope() {
    sfr::GuestMemory memory;
    sfr::NuiSkeletonEmulation first, second;
    sfr::NuiPlayerRouting routing;
    const std::optional<sfr::GamepadState> idle = sfr::GamepadState{};
    first.identify(); second.identify();
    for (unsigned i = 0; i < 30; ++i) routing.update(memory, first, second, {}, idle, false, false);
    const auto ready = first.hand(true);
    routing.observe_menu_page(0, 1, 1);
    routing.update(memory, first, second, {}, idle, false, false);
    require(first.hand(true) == ready, "first observed page must preserve the identity activation pose");
    routing.observe_menu_page(0, 2, 1);
    routing.update(memory, first, second, {}, idle, false, false);
    require(first.hand(true)[1] < 0 && first.hand(true)[2] == 2.45f,
            "new page must wait for intentional stick input rather than hover-confirm at centre");
    sfr::GamepadState sideways; sideways.thumb_rx = 32767;
    routing.update(memory, first, second, sideways, idle, false, false);
    for (unsigned i = 0; i < 30; ++i) routing.update(memory, first, second, {}, idle, false, false);
    const auto active = first.hand(true);
    routing.observe_menu_page(0, 3, 1);
    routing.update(memory, first, second, {}, std::nullopt, false, false);
    require(first.hand(true)[1] == active[1], "single-player page changes must retain existing behaviour");
    routing.update(memory, first, second, sideways, idle, false, false);
    require(first.hand(true)[1] > 0.4f,
            "a page rearm must survive a transient P2 disconnect until dual-pad input resumes");
    for (unsigned i = 0; i < 30; ++i) routing.update(memory, first, second, {}, idle, false, false);
    routing.observe_menu_page(0, 4, 1);
    routing.update(memory, first, second, {}, idle, false, true);
    require(first.hand(true)[1] == active[1], "camera page changes must not rearm the emulated hand");
    routing.observe_menu_page(1,20,58);
    routing.observe_menu_page(1,21,58,true);
    routing.update(memory,first,second,{},sideways,false,true);
    for(unsigned i=0;i<40;++i) routing.update(memory,first,second,{},idle,false,true);
    require(second.hand(true)[1]>0.4f,"P1 camera mode must preserve P2's Gear cursor rearm and acknowledgement");
    routing.observe_menu_page(1,21,58,false);
    routing.update(memory,first,second,{},idle,false,true);
    require(second.hand(true)[1]<0.3f,"P2 must finish its raise independently while P1 uses camera");
    routing.update(memory,first,second,sideways,idle,false,false,true);
    require(first.hand(true)[1]>0.4f,"camera-to-controller handoff must rearm P1's menu hand before stick input");
    routing.observe_menu_page(0, 5, 1);
    sfr::GamepadState trick; trick.thumb_ry = 32767;
    routing.update(memory, first, second, trick, idle, true, false);
    require(first.hand(true)[1] > 0.7f && first.hand(false)[1] > 0.7f,
            "queued menu rearm must not suppress race arm gestures");
}
void delayed_cursor_activation() {
    sfr::GuestMemory memory;
    memory.map(0x83E50000, 0x4000);
    memory.map(0x10000, 0x5000);
    constexpr uint32_t manager=0x10000, body0=0x11000, body1=0x12000, data0=0x13000, data1=0x14000;
    memory.store<uint32_t>(0x83E52F88,manager);
    memory.store<uint32_t>(manager+120,body0); memory.store<uint32_t>(manager+124,body1);
    memory.store<uint32_t>(body0+768,data1); memory.store<uint32_t>(body1+768,data0);
    memory.store<uint32_t>(data0,sfr::nui_tracked); memory.store<uint32_t>(data0+4,1);
    memory.store<uint32_t>(data1,sfr::nui_tracked); memory.store<uint32_t>(data1+4,2);
    sfr::NuiSkeletonEmulation slot0,slot1;
    sfr::NuiPlayerRouting routing;
    sfr::GamepadState move; move.thumb_rx=32767;
    const std::optional<sfr::GamepadState> idle=sfr::GamepadState{};
    routing.observe_menu_page(0,1,56); routing.observe_menu_page(1,2,56);
    routing.update(memory,slot0,slot1,move,move,false,false);
    for(unsigned i=0;i<35;++i) routing.update(memory,slot0,slot1,{},idle,false,false);
    routing.observe_menu_page(0,3,58,false); routing.observe_menu_page(1,4,58,true);
    routing.update(memory,slot0,slot1,move,move,false,false);
    // Real trace: P2 starts raising at frame 6273, but the game's menu
    // does not consume gestures again until frame 6936 (~11 seconds).
    for(unsigned i=0;i<350;++i) routing.update(memory,slot0,slot1,{},idle,false,false);
    require(slot0.hand(true)[1]>0.4f && slot1.hand(true)[1]<0.3f,
            "P2 must keep raising through a blocked Gear menu without freezing acknowledged P1");
    routing.observe_menu_page(1,4,58,false);
    routing.update(memory,slot0,slot1,{},idle,false,false);
    require(slot0.hand(true)[1]<0.3f,"guest cursor acknowledgement must finish P2's raise without Back");
    const auto centred=slot0.hand(true);
    routing.update(memory,slot0,slot1,{},move,false,false);
    require(slot0.hand(true)[0]>centred[0],"acknowledged P2 must resume normal cursor movement");
    routing.observe_menu_page(1,4,58,true);
    routing.update(memory,slot0,slot1,{},idle,false,false);
    require(slot0.hand(true)[1]<0.3f,"later hidden cursor must not restart an already completed raise");
    routing.observe_menu_page(1,5,58,true);
    routing.update(memory,slot0,slot1,{},move,false,false);
    for(unsigned i=0;i<40;++i) routing.update(memory,slot0,slot1,{},idle,false,false);
    sfr::GamepadState back; back.buttons=sfr::gamepad_button::back;
    routing.update(memory,slot0,slot1,{},back,false,false);
    require(slot0.hand(true)[1]<0 && slot0.hand(true)[2]==2.45f,"Back must cancel a pending raise");
    routing.update(memory,slot0,slot1,{},move,false,false);
    routing.observe_menu_page(1,0,0);
    for(unsigned i=0;i<40;++i) routing.update(memory,slot0,slot1,{},idle,false,false);
    require(slot0.hand(true)[1]<0.3f,"leaving the page must clear its pending acknowledgement");
    routing.observe_menu_page(0,6,58,true);
    routing.update(memory,slot0,slot1,move,idle,false,false);
    for(unsigned i=0;i<40;++i) routing.update(memory,slot0,slot1,{},idle,false,true);
    require(slot0.hand(true)[1]<0.3f && slot1.hand(true)[1]<0.3f,"camera mode must not wait for emulated cursor acknowledgement");
}
void guest_player_order() {
    sfr::GuestMemory memory;
    memory.map(0x83E50000, 0x4000);
    memory.map(0x10000, 0x8000);
    constexpr uint32_t manager = 0x10000, body0 = 0x11000, body1 = 0x12000;
    constexpr uint32_t data0 = 0x13000, data1 = 0x14000, frame = 0x15000;
    memory.store<uint32_t>(0x83E52F88, manager);
    memory.store<uint32_t>(manager + 120, body0);
    memory.store<uint32_t>(manager + 124, body1);
    memory.store<uint32_t>(body0 + 768, data1);
    memory.store<uint32_t>(body1 + 768, data0);
    memory.store<uint32_t>(data0, sfr::nui_tracked);
    memory.store<uint32_t>(data1, sfr::nui_tracked);
    memory.store<uint32_t>(data0 + 4, 1);
    memory.store<uint32_t>(data1 + 4, 2);
    sfr::NuiSkeletonEmulation slot0, slot1;
    slot0.identify(0); slot1.identify();
    sfr::NuiPlayerRouting routing;
    const std::optional<sfr::GamepadState> idle = sfr::GamepadState{};
    for (unsigned i = 0; i < 30; ++i) routing.update(memory, slot0, slot1, {}, idle, false, false);
    require(routing.reversed(), "guest P1 tracking ID 2 must reverse the pad-to-skeleton route");
    // A confirmed character leaves the hand low. Gear recreates its cursor;
    // the next stick movement must raise the hand again without BACK.
    routing.observe_menu_page(0, 0x20000, 1);
    routing.observe_menu_page(1, 0x21000, 1);
    sfr::GamepadState down; down.thumb_ry = -32767;
    for (unsigned i = 0; i < 60; ++i) routing.update(memory, slot0, slot1, down, down, false, false);
    require(slot1.hand(true)[1] < 0, "character confirmation fixture must leave P1's hand low");
    const auto waiting_second = slot0.hand(true);
    routing.observe_menu_page(0, 0, 0);
    routing.observe_menu_page(0, 0x22000, 2);
    sfr::GamepadState sideways; sideways.thumb_rx = 32767;
    routing.update(memory, slot0, slot1, sideways, idle, false, false);
    require(slot1.hand(true)[1] > 0.4f && slot0.hand(true) == waiting_second,
            "Gear entry must re-raise only the correct player's cursor on right-stick input");
    for (unsigned i = 0; i < 40; ++i) {
        routing.observe_menu_page(0, 0x22000, 2);
        routing.update(memory, slot0, slot1, sideways, idle, false, false);
    }
    require(slot1.hand(true)[0] > 0.2f && slot1.hand(true)[1] < 0.4f,
            "a stable Gear page must not restart the raise on every frame");
    // Reused page addresses still represent a new entry after an inactive gap.
    routing.observe_menu_page(1, 0, 0);
    routing.observe_menu_page(1, 0x21000, 1);
    const auto active_first = slot1.hand(true);
    routing.update(memory, slot0, slot1, {}, sideways, false, false);
    require(slot0.hand(true)[1] > 0.4f && slot1.hand(true) == active_first,
            "P2 must rearm independently even when the guest reuses its page address");
    for (unsigned i = 0; i < 30; ++i) routing.update(memory, slot0, slot1, {}, idle, false, false);
    const auto start0 = slot0.hand(true), start1 = slot1.hand(true);
    sfr::GamepadState first;
    first.thumb_rx = 32767;
    routing.update(memory, slot0, slot1, first, idle, false, false);
    require(slot0.hand(true) == start0 && slot1.hand(true)[0] > start1[0],
            "P1 must move the skeleton selected by the guest's P1 cursor, not skeleton slot zero");
    const auto moved1 = slot1.hand(true);
    sfr::GamepadState second;
    second.thumb_rx = -32767;
    routing.update(memory, slot0, slot1, {}, second, false, false);
    require(slot0.hand(true)[0] < start0[0] && slot1.hand(true) == moved1,
            "P2 must move only the guest's P2 cursor");
    sfr::NuiSkeletonEmulation::write_header(memory, frame, 1, 0);
    slot0.write_slot(memory, frame, 0, 1); slot1.write_slot(memory, frame, 1, 2);
    require(memory.load<uint32_t>(frame + sfr::nui_skeleton_data_offset + 4) == 1 &&
            memory.load<uint32_t>(frame + sfr::nui_skeleton_data_offset + sfr::nui_skeleton_data_size + 4) == 2,
            "routing must not swap skeleton slots or tracking identities");
    memory.store<uint32_t>(manager + 124, 0);
    routing.update(memory, slot0, slot1, {}, idle, false, false);
    require(routing.reversed(), "a partial Loading binding must retain the established order");
    memory.store<uint32_t>(0x83E52F88, 0);
    first = {}; first.thumb_ry = 32767;
    routing.update(memory, slot0, slot1, first, idle, true, false);
    require(slot1.hand(true)[1] > 0.7f && slot0.hand(true)[1] < 0,
            "Loading and race hand gestures must follow the same logical player");
    memory.store<uint32_t>(0x83E52F88, manager);
    memory.store<uint32_t>(manager + 124, body1);
    memory.store<uint32_t>(body0 + 768, data0);
    memory.store<uint32_t>(body1 + 768, data1);
    routing.update(memory, slot0, slot1, {}, idle, false, false);
    require(!routing.reversed(), "a valid normal guest order must not be hard-coded as swapped");
    memory.store<uint32_t>(body0 + 768, data1);
    memory.store<uint32_t>(body1 + 768, data0);
    routing.update(memory, slot0, slot1, {}, idle, false, false);
    routing.update(memory, slot0, slot1, {}, idle, false, true);
    require(routing.reversed(), "camera P1 must follow the guest binding instead of forcing skeleton slot zero");
    std::array<std::array<float,3>,sfr::nui_joint_count> camera{};
    for(uint32_t joint=0;joint<sfr::nui_joint_count;++joint)
        camera[joint]={float(joint)*0.03f,float(joint)*0.05f,2.0f+float(joint)*0.02f};
    const auto verify_camera=[&](uint32_t camera_slot,bool second_present) {
        sfr::NuiSkeletonEmulation::write_header(memory,frame,2,0);
        routing.write_slots(memory,frame,slot0,slot1,second_present,&camera);
        const auto data=frame+sfr::nui_skeleton_data_offset+camera_slot*sfr::nui_skeleton_data_size;
        require(memory.load<uint32_t>(data)==sfr::nui_tracked &&
                memory.load<uint32_t>(data+4)==camera_slot+1 &&
                memory.load<uint32_t>(data+12)==camera_slot,
                "camera routing must preserve the bound slot's tracking identity and user index");
        require(memory.load<uint32_t>(data+8)==(camera_slot? sfr::NuiSkeletonEmulation::guest : 0u),
                "camera routing must preserve the slot's enrollment");
        for(uint32_t joint=0;joint<sfr::nui_joint_count;++joint)
            for(uint32_t axis=0;axis<3;++axis)
                require(memory.load<uint32_t>(data+32+16*joint+4*axis)==std::bit_cast<uint32_t>(camera[joint][axis]),
                        "all camera XYZ coordinates must reach logical P1 unchanged");
        const uint32_t other_slot=1-camera_slot;
        const auto other=frame+sfr::nui_skeleton_data_offset+other_slot*sfr::nui_skeleton_data_size;
        require(memory.load<uint32_t>(other)==(second_present?sfr::nui_tracked:0u),
                "camera routing must not duplicate a body when the other controller disconnects");
        if(second_present) {
            const auto hand=(other_slot?slot1:slot0).hand(true);
            for(uint32_t axis=0;axis<3;++axis)
                require(memory.load<uint32_t>(other+32+16*sfr::nui_joint::hand_right+4*axis)==std::bit_cast<uint32_t>(hand[axis]),
                        "the other logical player must retain its own pad-driven hand");
        }
    };
    verify_camera(1,true);
    routing.update(memory,slot0,slot1,{},std::nullopt,false,true);
    verify_camera(1,false);
    memory.store<uint32_t>(manager+120,0);
    routing.update(memory,slot0,slot1,{},std::nullopt,true,true);
    verify_camera(1,false);
    memory.store<uint32_t>(manager+120,body0);
    memory.store<uint32_t>(body0+768,data0);
    memory.store<uint32_t>(body1+768,data1);
    routing.update(memory,slot0,slot1,{},idle,false,true);
    verify_camera(0,true);
    memory.store<uint32_t>(body0+768,data1);
    memory.store<uint32_t>(body1+768,data0);
    first = {}; first.thumb_rx = 32767;
    routing.update(memory, slot0, slot1, first, idle, false, false);
    for (unsigned i = 0; i < 30; ++i) routing.update(memory, slot0, slot1, {}, idle, false, false);
    routing.update(memory, slot0, slot1, {}, std::nullopt, false, false);
    require(routing.reversed(), "P2 disconnect must not change the established P1 skeleton");
    const auto disconnected0 = slot0.hand(true), connected1 = slot1.hand(true);
    first = {}; first.thumb_rx = 32767;
    routing.update(memory, slot0, slot1, first, std::nullopt, false, false);
    require(slot0.hand(true) == disconnected0 && slot1.hand(true)[0] > connected1[0],
            "connected P1 must continue moving its own cursor after P2 disconnects");
    require(!routing.slot_present(0, false) && routing.slot_present(1, false),
            "P2 disconnect must remove only P2's skeleton, preserving P1's tracking ID 2");
    memory.store<uint32_t>(0x83E52F88, 0);
    sfr::NuiPlayerRouting single_player;
    single_player.update(memory, slot0, slot1, {}, std::nullopt, false, false);
    require(!single_player.reversed() && single_player.slot_present(0, false) &&
            !single_player.slot_present(1, false), "fresh single-player startup must still use slot zero");
}
}
int main() {
    try { run(); guest_player_order(); menu_rearm_scope(); delayed_cursor_activation(); std::cout << "Two-player menu checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
