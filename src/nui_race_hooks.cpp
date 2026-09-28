#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "nui_race.h"
#include "camera_input.h"
#include "camera_race_motion.h"
#include <bit>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <vector>

// Race controls (see nui_race.h). Gesture detectors are called as
// detect(detector, source, results): source's vtable[1] returns the player's
// body record, results = {entries, ?, index, second index} with 84-byte
// entries whose words +4 and +20 collect the recognized gesture bits. A
// detector returns 1 when it recognized its gesture, 2 when it saw the
// opposite state (e.g. hands not up) and 0 otherwise. The bits each gesture
// reports are the ones the original detectors set.

namespace {
// One set of controls per player: the first from the pad the Kinect
// emulation calls the first player's, the second from the pad beside them.
sfr::RaceInput race[2];
sfr::CameraRaceMotion camera_motion;
uint64_t camera_last_tick=0;
// A body record each. The title keeps one pointer for all of its players
// (the Kinect manager's +0x78), but each player object holds its own at +4,
// which is the only way two of them can feel different things.
constexpr uint32_t body_address = 0x71730000, body_mapping = 0x2000;
constexpr uint32_t nui_box = 0x83E52F88, race_flag = 0x83E52F8C, time_global = 0x83E516A0;
bool swapped = false;
uint32_t original_body = 0;
uint32_t active_box = 0;
bool two_players = false;

sfr::GuestMemory& memory() { return *sfr::active_memory; }
float load_float(uint32_t address) { return std::bit_cast<float>(memory().load<uint32_t>(address)); }
void store_float(uint32_t address, float value) { memory().store<uint32_t>(address, std::bit_cast<uint32_t>(value)); }

// Legacy controller timing fields. During normal play +4 can be 1; Camera
// dwell durations use the host monotonic clock instead of assuming seconds.
float frame_seconds() {
    const uint32_t clock = memory().load<uint32_t>(time_global);
    return clock ? load_float(memory().load<uint32_t>(clock + 24) + 4) : 1.0f / 60.0f;
}
float frame_count() {
    const uint32_t clock = memory().load<uint32_t>(time_global);
    return clock ? load_float(memory().load<uint32_t>(clock + 24) + 40) : 1.0f;
}

bool pad_racing() { return swapped; }
bool camera_player(uint32_t player) {
    return sfr::camera_controls_player(player,sfr::camera_motion_active());
}
// A real Kinect's race: the title reads its bodies through its own record
// and detectors, but steers by the lean pair (+640 / +644), which on the
// console are pixel counts from the depth view. No depth image reaches the
// title here, so the first player's lean is taken from the body the camera
// motion's way (the torso's roll against a calibrated stance) and written as
// that pair.
bool sensor_steering = false;
uint32_t sensor_record = 0;
void write_camera_lean(uint32_t record,float scale=1.f) {
    // These are depth-image pixel counts in the original game, not joints.
    // RGB pose input supplies their ratio; zero/zero means full right lean.
    const float lean=camera_motion.lean()*scale;
    store_float(record+640,1.f+std::max(lean,0.f));
    store_float(record+644,1.f+std::max(-lean,0.f));
}
// A full lean of the sensor's body is this ratio minus one: the title reads
// up to 3.5 from the depth view, so the default uses its whole range (a
// full lean at 1 turned the board only a little). SFR_KINECT_LEAN_SCALE
// changes it, 0.1..3.5.
float sensor_lean_scale() {
    static const float scale=[] {
        const char* text=std::getenv("SFR_KINECT_LEAN_SCALE");
        return std::clamp(text && *text?std::strtof(text,nullptr):3.5f,0.1f,3.5f);
    }();
    return scale;
}
void update_camera_motion(uint32_t record,uint64_t generation) {
    const uint64_t now=sfr::camera_motion_clock_ns();
    const float seconds=camera_last_tick && now>camera_last_tick?float(double(now-camera_last_tick)*1e-9):1.f/60.f;
    camera_last_tick=now;
    const auto point=[&](uint32_t offset) {
        return std::array{load_float(record+offset),load_float(record+offset+4),load_float(record+offset+8)};
    };
    const bool ready=camera_motion.ready();
    camera_motion.observe({point(0),point(32),point(224),point(288),point(64),point(128),point(192),point(256),point(80),point(96),point(144),point(160),point(208),point(272)},seconds,generation);
    if(camera_motion.ready()!=ready)
        std::cerr<<"CAMERA_RACE_CALIBRATION ready="<<camera_motion.ready()<<'\n';
}

// The body record the title itself would read for the player a detector is
// being asked about. Every original detector begins the same way (822C9050
// and the rest): the source holds an object, and the second entry of that
// object's vtable returns that player's record.
//
//   lwz r3,0(r4)   ; the player object behind the source
//   lwz r11,0(r3)  ; its vtable
//   lwz r10,4(r11) ; vtable[1]
//   bctrl          ; -> the record, in r3
//
// This is the only way a second player's record can be reached: the Kinect
// manager holds exactly one record pointer (+0x78) whatever the number of
// players, as a dump of all 8208 of its bytes shows.
uint32_t body_of_source(PPCContext& ctx, uint8_t* base, uint32_t source, uint32_t* which = nullptr) {
    if (!source) return 0;
    auto& m = memory();
    const uint32_t object = m.load<uint32_t>(source);
    if (!object) return 0;
    const uint32_t vtable = m.load<uint32_t>(object);
    if (!vtable) return 0;
    const uint32_t method = m.load<uint32_t>(uint64_t(vtable) + 4);
    if (!method) return 0;
    if (which) *which = method;
    // The reader used by the title's race objects is exactly
    // lwz r3,4(r3); blr. Read its live pointer without a guest call.
    if (method == 0x82918418) return m.load<uint32_t>(uint64_t(object) + 4);
    PPCContext saved = ctx;
    ctx.r3.u64 = object;
    sfr::call_indirect(ctx, base, method);
    const uint32_t record = ctx.r3.u32;
    ctx = saved;
    return record;
}

// SFR_RACE_BODY_SOURCES=1: the distinct records the detectors are asked
// about, and whether each is the one we put in the manager. With one player
// there should be one; a second player is the whole point of asking.
void note_detector_source(PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t source) {
    static const bool wanted = [] { const char* t = std::getenv("SFR_RACE_BODY_SOURCES");
                                    return t && *t != '0'; }();
    if (!wanted) return;
    // Keyed by the source, not the record: two players' objects have both
    // answered with the same record, so the record alone hides how many
    // players are being asked about.
    static std::unordered_map<uint32_t, uint64_t> seen;
    uint32_t method = 0;
    const uint32_t record = body_of_source(ctx, base, source, &method);
    const uint64_t count = ++seen[source];
    if (count == 1 || (count & (count - 1)) == 0)
        std::cerr << "RACE_BODY_SOURCE detector=0x" << std::hex << address << " source=0x" << source
                  << " object=0x" << memory().load<uint32_t>(source) << " reader=0x" << method
                  << " record=0x" << record << " ours=0x" << body_address << " original=0x" << original_body
                  << " at_78=0x" << (memory().load<uint32_t>(nui_box) ? memory().load<uint32_t>(memory().load<uint32_t>(nui_box) + 0x78) : 0)
                  << std::dec << " count=" << count << " sources=" << seen.size() << char(10);
}

// Result entry the detector writes: [results] + [results+8 (or +12)] * 84.
uint32_t entry(uint32_t results, uint32_t index_offset = 8) {
    return memory().load<uint32_t>(results) + memory().load<uint32_t>(results + index_offset) * 84;
}
void set_bits(uint32_t address, uint32_t bits) { memory().store<uint32_t>(address, memory().load<uint32_t>(address) | bits); }
void set_byte(uint32_t address, uint8_t value) { memory().store<uint8_t>(address, value); }

// Only gestures whose inputs are missing/unreliable in RGB pose need a bridge.
// All joint XYZ and unrelated camera detectors keep their original path.
std::optional<uint32_t> camera_gesture(uint32_t address,uint32_t detector,uint32_t results) {
    const auto primary=[&](uint32_t bits){set_bits(entry(results)+4,bits);};
    switch(address) {
    case 0x822C9050:
        memory().store<uint32_t>(detector+88,camera_motion.jump()?0xFFFFFFFFu:1u);
        memory().store<uint32_t>(detector+92,camera_motion.jump()?0xFFFFFFFFu:1u);
        if(camera_motion.jump()){primary(0x200);return 1;}
        return 2;
    case 0x822C8778: case 0x822CB840:
        if(camera_motion.crouch()){primary(0x7000);return 1;}
        return 2;
    case 0x822C9A80: case 0x822CAF48:
        if(camera_motion.boost()){primary(0x8000);return 1;}
        return 0;
    case 0x822CA6B0:
        if(!camera_motion.brake_ready()){set_bits(entry(results)+20,0x2000);return 2;}
        break;
    case 0x822C9BF0:
        if(!camera_motion.brake_ready()){set_byte(entry(results)+80,0);store_float(detector+72,0);return 2;}
        break;
    case 0x822CA518:
        if(camera_motion.jump_protected()||(camera_motion.arm_action()&&!camera_motion.kick_leg_action())) {
            set_byte(detector+72,0);return 2;
        }
        break;
    case 0x822CBD28:
        if(camera_motion.jump_protected()||(camera_motion.arm_action()&&!camera_motion.kick_leg_action())) {
            memory().store<uint32_t>(detector+72,0);return 2;
        }
        break;
    }
    return std::nullopt;
}

// Which player a detector is being asked about, and where that player's
// body record lives.
//
// A two-player race asks through four objects, two for each player, and a
// log of a real one says how to tell them apart: the first player's objects
// answer with the Kinect manager's record -- ours, because we put it there
// -- and the second player's answer with one of their own. So the record an
// object answers with identifies the player. Loading can replace an object
// behind the same source, or change its record, so resolve the live pointer
// each time. The original manager record is also a first-player alias.
//
// The second player's own record is also where their controls go: the title
// built it and keeps it up to date, and writing the pad's fields over it
// leaves everything else exactly as the title left it.
// This map only suppresses repeated diagnostics; no stored address is ever
// dereferenced later. Every body write requires a current guest consumer.
std::unordered_map<uint32_t, uint32_t> source_records;
// Valid only on the stack of a live guest race consumer. Never replay a
// saved source after that call: Loading can free it without changing the
// manager or body record.
thread_local uint32_t live_race_source = 0;
struct RaceSourceScope {
    uint32_t previous = live_race_source;
    explicit RaceSourceScope(uint32_t source) { live_race_source = source; }
    ~RaceSourceScope() { live_race_source = previous; }
};

uint32_t player_of_record(uint32_t record) {
    // One player's race is asked about through two objects of their own, so
    // telling players apart is only right when there really are two.
    if (!two_players) return 0;
    // SFR_RACE_TWO_PLAYERS=0 puts both riders back on the first pad, which
    // is what they shared before any of this.
    static const bool split = [] { const char* t = std::getenv("SFR_RACE_TWO_PLAYERS");
                                   return !t || !*t || *t != '0'; }();
    if (!split) return 0;
    static const bool swap = [] { const char* t = std::getenv("SFR_RACE_PLAYERS_SWAP");
                                  return t && *t && *t != '0'; }();
    const uint32_t player = record && record != body_address && record != original_body ? 1u : 0u;
    return swap ? 1u - player : player;
}

uint32_t player_of_source(PPCContext& ctx, uint8_t* base, uint32_t source) {
    if (!two_players) return 0;
    const uint32_t record = body_of_source(ctx, base, source);
    const auto previous = source_records.find(source);
    if (record && record != body_address && record != original_body &&
        (previous == source_records.end() || previous->second != record))
        std::cerr << "NUI_RACE_SECOND_BODY record=0x" << std::hex << record << " source=0x" << source
                  << std::dec << char(10);
    source_records[source] = record;
    const uint32_t player = player_of_record(record);
    if (record && !camera_player(player)) race[player].write(memory(), record);
    return player;
}

const sfr::RaceBody& body() { return race[0].body(); }

}

PPC_FUNC_IMPL(__imp__sub_82438930);

// Kinect manager update, once per frame before it handles the players: in a
// race ([83E52F8C] nonzero) P1's body record (KinnectNuiBox+0x78) is ours,
// refreshed from the pad; afterwards the title's record is put back.
SFR_HOOK(sub_82438930) {
    sfr::enter_function(ctx, "sub_82438930", 0x82438930);
    auto& m = memory();
    const uint32_t box = m.load<uint32_t>(nui_box);
    // SFR_NO_RACE_BODY leaves the title's own body record in place.
    static const bool disabled = std::getenv("SFR_NO_RACE_BODY") != nullptr;
    // A real Kinect's bodies are the title's to read, as on the console.
    const bool racing = !disabled && !sfr::nui_body_from_sensor() && m.load<uint32_t>(race_flag) != 0;
    const float seconds = frame_seconds();
    if(camera_player(0)) race[0]=sfr::RaceInput{};
    else race[0].update(sfr::nui_gamepad(), seconds);
    const auto second_pad = sfr::second_player_pad();
    if (second_pad) race[1].update(*second_pad, seconds);
    else race[1] = sfr::RaceInput{};  // loss of a pad is not an A/X release gesture
    if (box && racing) {
        if (m.available(body_address, body_mapping)) {
            m.map(body_address, body_mapping);
            for (uint32_t offset = 0; offset < body_mapping; offset += 4) m.store<uint32_t>(body_address + offset, 0);
        }
        const uint32_t current = m.load<uint32_t>(box + 0x78);
        if (box != active_box || (current != body_address && current != original_body)) {
            source_records.clear();
            camera_motion.reset();
            camera_last_tick=0;
        }
        active_box = box;
        // A disconnected second pad becomes neutral, never the first pad.
        // Manager/body replacement during Loading does not end ownership;
        // only leaving the race clears it.
        two_players = two_players || second_pad.has_value();
        if (current != body_address) {
            original_body = current;
            m.store<uint32_t>(box + 0x78, body_address);
            std::cerr << "NUI_RACE_BODY installed original=0x" << std::hex << current << std::dec << '\n';
        }
        // Every field the title keeps (pointers, joint positions) is the one
        // it built, refreshed each frame: copying once left the body frozen in
        // the pose it had when the race began, a raised menu cursor hand
        // included. Only the fields below are the pad's.
        if (original_body)
            for (uint32_t offset = 0; offset < sfr::race_body_size; offset += 4)
                m.store<uint32_t>(body_address + offset, m.load<uint32_t>(original_body + offset));
        swapped = true;
        const uint32_t player=player_of_record(body_address);
        if(!camera_player(player)) race[player].write(m, body_address);
        // SFR_RACE_BODY_DUMP=N: every N frames, the record's non-zero floats.
        // The title fills the rest from the skeleton, so this shows what the
        // race reads besides the pad's fields.
        static const uint32_t dump = [] { const char* t = std::getenv("SFR_RACE_BODY_DUMP");
                                          return t ? uint32_t(std::strtoul(t, nullptr, 10)) : 0u; }();
        static uint32_t frames = 0;
        if (dump && frames++ % dump == 0) {
            std::cerr << "NUI_RACE_BODY_FIELDS frame=" << frames;
            for (uint32_t offset = 0; offset < sfr::race_body_size; offset += 4) {
                const uint32_t word = m.load<uint32_t>(body_address + offset);
                const float value = std::bit_cast<float>(word);
                if (word && std::isfinite(value) && std::fabs(value) < 1e6f)
                    std::cerr << ' ' << offset << '=' << value;
                else if (word)
                    std::cerr << ' ' << offset << "=0x" << std::hex << word << std::dec;
            }
            std::cerr << '\n';
        }
    } else if (swapped) {
        if (box && m.load<uint32_t>(box + 0x78) == body_address) m.store<uint32_t>(box + 0x78, original_body);
        swapped = false;
        source_records.clear();  // no active manager/body for these diagnostics
        active_box = 0;
        std::cerr << "NUI_RACE_BODY restored original=0x" << std::hex << original_body << std::dec << '\n';
    }
    // Loading can briefly remove the manager while the race remains active.
    // Preserve ownership then, but clear it on the real exit even if that
    // missing-manager frame already removed the injected body.
    if (!racing) two_players = false;
    __imp__sub_82438930(ctx, base);
    // Sample the live original skeleton after the manager refresh, once per
    // game update. Never advance gesture timers once per detector invocation.
    const bool was_steering=sensor_steering;
    sensor_steering=!disabled && sfr::nui_body_from_sensor() && box && m.load<uint32_t>(race_flag)!=0 &&
                    m.load<uint32_t>(box+0x78);
    sensor_record=sensor_steering?m.load<uint32_t>(box+0x78):0;
    if(sensor_steering!=was_steering)
        std::cerr<<"NUI_RACE_SENSOR_LEAN active="<<sensor_steering<<" record=0x"<<std::hex<<sensor_record<<std::dec<<'\n';
    if(racing && box && original_body && camera_player(0))update_camera_motion(original_body,sfr::camera_pose_generation());
    else if(sensor_steering) {
        update_camera_motion(sensor_record,sfr::kinect_frame_generation());
        write_camera_lean(sensor_record,sensor_lean_scale());
    }
    else {camera_motion.reset();camera_last_tick=0;}
    static const bool motion_trace=[] {const char* p=std::getenv("SFR_CAMERA_RACE_TRACE");return p && *p=='1';}();
    static uint32_t motion_frames=0;
    if(motion_trace && (racing || sensor_steering) && (++motion_frames%30==0 || camera_motion.jump() || camera_motion.overthrow())) {
        std::cerr<<"CAMERA_RACE_MOTION title_step="<<seconds<<" generation="<<sfr::camera_pose_generation()
                 <<" camera="<<camera_player(0)<<" ready="<<camera_motion.ready()
                 <<" lean="<<camera_motion.lean()<<" crouch="<<camera_motion.crouch()
                 <<" jump="<<camera_motion.jump()<<" acceleration="<<camera_motion.boost()<<" arm_action="<<camera_motion.arm_action()
                 <<" overthrow="<<camera_motion.overthrow()<<" kick_leg="<<camera_motion.kick_leg_action()<<'\n';
    }
}

PPC_FUNC_IMPL(__imp__sub_822C6200);
PPC_FUNC_IMPL(__imp__sub_82918418);

// This original race consumer updates source->object through vtable[2],
// then reads its body through vtable[1] before calculating lean. Keep that
// order and the title's calculations; patch only the live reader result.
SFR_HOOK(sub_822C6200) {
    sfr::enter_function(ctx, "sub_822C6200", 0x822C6200);
    RaceSourceScope scope(pad_racing() || sensor_steering ? ctx.r3.u32 : 0);
    __imp__sub_822C6200(ctx, base);
}

// 82918418 is a shared accessor, so calls outside the live race source's
// update retain their original behavior. The scope restores on exceptions
// and is thread-local because guest consumers can run on different threads.
SFR_HOOK(sub_82918418) {
    sfr::enter_function(ctx, "sub_82918418", 0x82918418);
    const uint32_t object = ctx.r3.u32;
    __imp__sub_82918418(ctx, base);
    const uint32_t record = ctx.r3.u32;
    // The depth view's worker can refill the pair between the manager's
    // update and this read: a sensor's first player gets its lean again here.
    if (sensor_steering && live_race_source && record && record == sensor_record &&
        memory().load<uint32_t>(live_race_source) == object) {
        write_camera_lean(record);
        return;
    }
    if (pad_racing() && live_race_source && record &&
        memory().load<uint32_t>(live_race_source) == object) {
        const uint32_t player=player_of_record(record);
        if(camera_player(player))write_camera_lean(record);
        else race[player].write(memory(), record);
    }
}

// Opt-in detector transitions: numeric state only, no camera images.
static void trace_camera_detector(uint32_t address,uint32_t detector,uint32_t results,uint32_t result) {
    if(address!=0x822C8778 && address!=0x822CB840 && address!=0x822CA6B0 && address!=0x822C9BF0 && address!=0x822C9050 &&
       address!=0x822CA518 && address!=0x822CBD28 && address!=0x822CC590 && address!=0x822CCD98 && address!=0x822CD068)return;
    static const bool enabled=[] {const char* p=std::getenv("SFR_CAMERA_RACE_TRACE");return p && *p=='1';}();
    if(!enabled)return;
    static std::unordered_map<uint64_t,std::array<uint32_t,4>> last;
    const uint64_t key=uint64_t(address)<<32|detector;
    const uint32_t left_arm=address==0x822CC590?memory().load<uint32_t>(detector+72):0;
    const uint32_t right_arm=address==0x822CC590?memory().load<uint32_t>(detector+76):0;
    const uint32_t kick_state=address==0x822CA518?memory().load<uint8_t>(detector+72):
        address==0x822CBD28?memory().load<uint32_t>(detector+72):0;
    const std::array state{result,left_arm,right_arm,kick_state};
    const bool changed=!last.contains(key)||last[key]!=state;
    last[key]=state;
    if(changed||camera_motion.jump())std::cerr<<"CAMERA_GESTURE address=0x"<<std::hex<<address
        <<" detector=0x"<<detector<<" primary=0x"<<memory().load<uint32_t>(entry(results)+4)
        <<" secondary=0x"<<memory().load<uint32_t>(entry(results)+20)<<std::dec
        <<" arm_first="<<left_arm<<" arm_second="<<right_arm<<" result="<<result<<" generation="<<sfr::camera_pose_generation()<<" motion_jump="<<camera_motion.jump()<<" brake_ready="<<camera_motion.brake_ready()<<" arm_action="<<camera_motion.arm_action()
        <<" kick_state="<<kick_state<<" kick_leg="<<camera_motion.kick_leg_action()<<'\n';
}

// Opt-in numeric consumer trace: distinguish recognized crouch from title
// restrictions (for example the opening tutorial masks other race actions).
PPC_FUNC_IMPL(__imp__sub_822B60F8);
SFR_HOOK(sub_822B60F8) {
    sfr::enter_function(ctx,"sub_822B60F8",0x822B60F8);
    const uint32_t actor=ctx.r3.u32;
    __imp__sub_822B60F8(ctx,base);
    static const bool enabled=[] {const char* p=std::getenv("SFR_CAMERA_RACE_TRACE");return p&&*p=='1';}();
    if(!enabled || !pad_racing() || !sfr::camera_motion_active())return;
    const uint32_t owner=memory().load<uint32_t>(actor+1680);
    if(!owner)return;
    const uint32_t results=memory().load<uint32_t>(owner+3220);
    const uint32_t animation=memory().load<uint32_t>(owner+3208),state=memory().load<uint32_t>(owner+3212);
    if(!results || !animation || !state)return;
    const uint32_t entries=memory().load<uint32_t>(results+16),data=memory().load<uint32_t>(animation+12);
    if(!entries || !data)return;
    const uint32_t read=entries+84*memory().load<uint32_t>(results+28);
    const std::array<uint32_t,6> now{memory().load<uint32_t>(read+4),memory().load<uint32_t>(read+8),
        memory().load<uint32_t>(data+44),memory().load<uint32_t>(state+400),uint32_t(camera_motion.crouch()),uint32_t(camera_motion.jump())};
    static std::unordered_map<uint32_t,std::array<uint32_t,6>> previous;
    if(previous.contains(actor) && previous.at(actor)==now)return;
    previous.insert_or_assign(actor,now);
    std::cerr<<"CAMERA_RACE_ACTOR actor=0x"<<std::hex<<actor<<" ring=0x"<<results+16
             <<" written=0x"<<memory().load<uint32_t>(entries+84*memory().load<uint32_t>(results+24)+4)
             <<" flags=0x"<<now[0]<<" released=0x"<<now[1]
             <<" mask1=0x"<<memory().load<uint32_t>(results+36)<<" mask2=0x"<<memory().load<uint32_t>(results+40)<<std::dec
             <<" anim="<<now[2]<<" state="<<now[3]<<" crouch="<<now[4]<<" jump="<<now[5]
             <<" enabled="<<unsigned(memory().load<uint8_t>(actor+1989))<<" block="<<load_float(actor+2080)
             <<" block2636="<<unsigned(memory().load<uint8_t>(actor+2636))<<'\n';
}

void finish_camera_overthrow(uint32_t detector,uint32_t results,uint64_t& result) {
    if(result==1) {camera_motion.consume_overthrow();return;}
    if(!camera_motion.overthrow())return;
    set_bits(entry(results)+20,0x8);
    // Original +72/+76 are whole-word arm states, not power/strength bytes.
    memory().store<uint32_t>(detector+72,0);
    memory().store<uint32_t>(detector+76,0);
    camera_motion.consume_overthrow();result=1;
}

// One detector override: while the pad drives the race the body decides,
// otherwise the original detector runs.
#define RACE_DETECTOR(address, ...)                                      \
    PPC_FUNC_IMPL(__imp__sub_##address);                                       \
    SFR_HOOK(sub_##address) {                                                  \
        sfr::enter_function(ctx, "sub_" #address, 0x##address);               \
        if (!pad_racing()) { __imp__sub_##address(ctx, base); return; }        \
        const uint32_t detector = ctx.r3.u32, source = ctx.r4.u32, results = ctx.r5.u32; \
        (void)detector; (void)source; (void)results;                           \
        note_detector_source(ctx, base, 0x##address, source);                  \
        const uint32_t player=player_of_source(ctx,base,source);             \
        if(camera_player(player)) {                                        \
            if(const auto bridged=camera_gesture(0x##address,detector,results))ctx.r3.u64=*bridged; \
            else {                                                        \
                __imp__sub_##address(ctx,base);                            \
                if constexpr(0x##address==0x822CA518 || 0x##address==0x822CBD28) { \
                    /* Side and Kick share this prerequisite in 822C5BB0. */ \
                    /* A gated kick must never enter this branch. */      \
                    if(memory().load<uint32_t>(entry(results)+4)&0x1800000) \
                        set_bits(entry(results)+4,0x400000);               \
                }                                                         \
            }                                                             \
            if constexpr(0x##address==0x822CC590)finish_camera_overthrow(detector,results,ctx.r3.u64); \
            trace_camera_detector(0x##address,detector,results,ctx.r3.u32); \
            return;                                                       \
        }                                                                 \
        const sfr::RaceBody& b = race[player].body();                        \
        uint32_t result = 0;                                                   \
        __VA_ARGS__                                                            \
        ctx.r3.u64 = result;                                                   \
    }

// Jump: releasing A (after crouching with it). The detector's +88/+92 hold
// the jump direction, -1 on the jump frame and 1 otherwise.
RACE_DETECTOR(822C9050, {
    const bool jump = b.a_released != 0;
    memory().store<uint32_t>(detector + 88, jump ? 0xFFFFFFFFu : 1u);
    memory().store<uint32_t>(detector + 92, jump ? 0xFFFFFFFFu : 1u);
    if (jump) { set_bits(entry(results) + 4, 0x200); result = 1; } else result = 2;
})

// Crouch: A held for six frames.
namespace { std::unordered_map<uint32_t, float> crouch_frames; }
RACE_DETECTOR(822C8778, {
    float& held = crouch_frames[detector];
    if (b.a_held == 0) { held = 0; result = 2; }
    else {
        if (held < 6.0f) held += frame_count();
        if (held >= 6.0f) { set_bits(entry(results) + 4, 0x7000); result = 1; }
    }
})

// Charge: A held.
RACE_DETECTOR(822CB840, {
    if (b.a_held != 0) { set_bits(entry(results) + 4, 0x7000); result = 1; }
})

// Arms up: right stick up.
RACE_DETECTOR(822C8650, {
    if (b.right_y > 0) {
        set_bits(entry(results) + 4, 0x100000C0);
        set_bits(entry(results) + 20, 0x120);
        result = 1;
    } else result = 2;
})

// Flying skill: A held.
RACE_DETECTOR(822C9180, {
    if (b.a_held == 0) result = 2;
    else { set_bits(entry(results) + 4, 0x40000); result = 1; }
})

// Board acceleration (and its second detector): left stick pushed forward.
#define RACE_BOARD_ACCELERATION(address)                                       \
    RACE_DETECTOR(address, {                                                   \
        if (b.left_y >= 0.65f) { set_bits(entry(results) + 4, 0x8000); result = 1; } \
    })
RACE_BOARD_ACCELERATION(822C9A80)
RACE_BOARD_ACCELERATION(822CAF48)

// Side: B held. The original detector reads copied skeleton geometry and
// tracked hands, which A and stick-down also supply. A false Side result
// makes the title's group filter suppress crouch and jump (0x1203).
RACE_DETECTOR(822CA6B0, {
    if (b.b_held) { set_bits(entry(results) + 4, 0x400000); result = 1; }
    else { set_bits(entry(results) + 20, 0x2000); result = 2; }
})

// Brake: B held, harder with the left stick (+80 = strength in percent).
RACE_DETECTOR(822C9BF0, {
    const uint32_t e = entry(results);
    if (b.b_held) {
        set_byte(e + 80, uint8_t(std::fabs(b.left_y) * 100.0f));
        set_bits(e + 4, 0x00400100);
        result = 1;
    } else { set_byte(e + 80, 0); result = 2; }
})

// Grab (poles, rails): B held, full strength.
RACE_DETECTOR(822CB0B8, {
    const uint32_t e = entry(results);
    if (b.b_held) { set_byte(e + 80, 100); set_bits(e + 4, 0x100); result = 1; }
    else { set_byte(e + 80, 0); result = 2; }
})

// Kick dash (and dash): X pressed starts the kick, releasing X fires it.
#define RACE_KICK(address)                                                     \
    RACE_DETECTOR(address, {                                                   \
        if (b.x_pressed != 0) { set_bits(entry(results) + 4, 0x00C00000); result = 0; } \
        else if (b.x_released != 0) { set_bits(entry(results) + 4, 0x01400000); result = 1; } \
        else result = 2;                                                       \
    })
RACE_KICK(822CA518)
RACE_KICK(822CBD28)

// Power skill (and its second detector): LB left side, RB right side.
#define RACE_POWER_SKILL(address)                                              \
    RACE_DETECTOR(address, {                                                   \
        uint32_t bits = 0;                                                     \
        if (b.lb_pressed) bits |= 0x80000;                                     \
        if (b.rb_pressed) bits |= 0x100000;                                    \
        if (bits) { set_bits(entry(results) + 4, bits); result = 1; }         \
    })
RACE_POWER_SKILL(822CA810)
RACE_POWER_SKILL(822CBF30)

// Stance: Y switches between regular and goofy (+72 of the result). The
// source's +96/+100 name the front and back foot joints (title descriptors
// 8218B518 / 8218B4F8) and +104 the stance.
RACE_DETECTOR(822CAC90, {
    if (b.y_pressed != 0) {
        const uint32_t e = entry(results);
        const uint32_t goofy = memory().load<uint32_t>(e + 72) == 0 ? 1 : 0;
        memory().store<uint32_t>(e + 72, goofy);
        memory().store<uint32_t>(source + 104, goofy);
        memory().store<uint32_t>(source + 100, goofy ? 0x8218B518u : 0x8218B4F8u);
        memory().store<uint32_t>(source + 96, goofy ? 0x8218B4F8u : 0x8218B518u);
        set_bits(e + 4, 0x200000);
        result = 2;
    }
})

// Tricks: turning the left stick around turns the trick (+56 of the result
// accumulates half turns, +4 bit 0x800).
namespace { struct Turn { bool started = false; float angle = 0; }; std::unordered_map<uint32_t, Turn> turns; }
RACE_DETECTOR(822C9938, {
    result = 1;
    if (b.left_x != 0 || b.left_y != 0) {
        Turn& turn = turns[detector];
        const float angle = std::fmod(std::atan2(b.left_y, b.left_x) * 57.29578f + 360.0f, 360.0f);
        if (turn.started && angle != turn.angle) {
            float delta = angle - turn.angle;
            if (std::fabs(delta) > 180.0f) delta += delta > 0 ? -360.0f : 360.0f;
            const uint32_t e = entry(results);
            store_float(e + 56, load_float(e + 56) + delta / 180.0f);
            set_bits(e + 4, 0x800);
        }
        turn.started = true;
        turn.angle = angle;
    }
})

// Item and special-gear actions: the right trigger (+20 bits).
#define RACE_TRIGGER(address, bits, ...)                                       \
    RACE_DETECTOR(address, {                                                   \
        if (b.right_trigger > 0) { __VA_ARGS__; set_bits(entry(results) + 20, bits); result = 1; } \
    })
RACE_TRIGGER(822CC2D0, 0x1, {})                                                            // hammer
RACE_TRIGGER(822CC590, 0x8, { set_byte(detector + 72, 255); memory().store<uint32_t>(detector + 76, 1); })  // over throw
RACE_TRIGGER(822CCD98, 0x10, { memory().store<uint32_t>(detector + 76, 1); })             // under swing
RACE_TRIGGER(822CD068, 0x2, { memory().store<uint32_t>(detector + 72, 0); })              // under throw
RACE_TRIGGER(822CDDB0, 0x400, { set_byte(detector + 72, 255); })                          // inflator

// Shake: the right trigger shakes every third of a second while held.
namespace { std::unordered_map<uint32_t, float> shakes; }
RACE_DETECTOR(822CC8F0, {
    if (b.right_trigger == 0) result = 2;
    else {
        float& time = shakes[detector];
        time += frame_seconds();
        if (time >= 1.0f / 3.0f) { time = 0; set_bits(entry(results) + 20, 0x44); result = 1; }
    }
})

// Steam wipe: the right trigger wipes where the right stick points (+76 x,
// +78 y in screen units around the centre).
RACE_DETECTOR(822CDEE0, {
    if (b.right_trigger > 0) {
        set_bits(entry(results) + 20, 0x10000);
        auto move = [&](uint32_t offset, float axis, float limit) {
            const float step = axis * frame_count() * 14.0f;
            const float next = std::clamp(float(int16_t(memory().load<uint16_t>(detector + offset))) - step, -limit, limit);
            memory().store<uint16_t>(detector + offset, uint16_t(int16_t(next)));
        };
        move(78, b.right_y, 640.0f);
        move(76, -b.right_x, 480.0f);
    }
})

// Lever and handle gear: the left stick (+82 / +83 of the result, percent).
RACE_DETECTOR(822CD638, {
    if (b.left_x != 0) {
        const uint32_t e = entry(results);
        set_byte(e + 82, uint8_t(int8_t(b.left_x * 100.0f)));
        if (memory().load<uint8_t>(e + 82)) { set_bits(e + 20, 0x1000); result = 1; }
    }
})
RACE_DETECTOR(822CD960, {
    const uint32_t e = entry(results);
    if (b.left_x == 0) set_byte(e + 83, 0);
    else {
        set_byte(e + 83, uint8_t(int8_t(b.left_x * 100.0f)));
        if (memory().load<uint8_t>(e + 83)) { set_bits(e + 20, 0x4000); result = 1; }
    }
})
// Front curve gear: the lean, on the second result entry (+81).
RACE_DETECTOR(822CD3F0, {
    const uint32_t e = entry(results, 12);
    set_byte(e + 81, uint8_t(int8_t(-b.lean * 100.0f)));
    if (memory().load<uint8_t>(e + 81)) set_bits(e + 20, 0x800);
})

// Swimming: paddling by holding X (strokes every half second).
namespace { std::unordered_map<uint32_t, float> strokes; }
RACE_DETECTOR(822CE118, {
    float& stroke = strokes[detector];
    if (b.x_held_seconds <= 0) stroke = 0;
    else {
        stroke += frame_seconds();
        if (stroke >= 0.5f) { stroke = 0; set_bits(entry(results) + 20, 0x80); result = 1; }
    }
})

// The detector at 821A2484 (original 822CB9B8) uses the one at 821A2344
// (822C8958) while the pad drives the race.
PPC_FUNC_IMPL(__imp__sub_822CB9B8);
SFR_HOOK(sub_822CB9B8) {
    sfr::enter_function(ctx, "sub_822CB9B8", 0x822CB9B8);
    if (pad_racing() && !camera_player(player_of_source(ctx,base,ctx.r4.u32))) sub_822C8958(ctx, base);
    else __imp__sub_822CB9B8(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_822B72E0);

// Race preparation ("On your Gear!"), state at +2408: 0 starts it, 1 waits
// for the sensor, 2..6 run the body measurements of the chosen gear (each
// step is a pose the title reads from the skeleton), 7 ends the preparation
// and 8 races. A pad has no body to measure, so its preparation ends with
// the measurements skipped.
SFR_HOOK(sub_822B72E0) {
    sfr::enter_function(ctx, "sub_822B72E0", 0x822B72E0);
    const uint32_t race = ctx.r3.u32;
    __imp__sub_822B72E0(ctx, base);
    if (!pad_racing()) return;
    const uint32_t state = memory().load<uint32_t>(race + 2408);
    if (state >= 2 && state <= 6) {
        memory().store<uint32_t>(race + 2408, 7);
        std::cerr << "NUI_RACE_PREPARATION skipped=" << state << '\n';
    }
}
