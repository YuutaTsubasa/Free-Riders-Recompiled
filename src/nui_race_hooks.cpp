#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include "guest_memory.h"
#include "nui_race.h"
#include "camera_input.h"
#include "camera_race_motion.h"
#include "kinect_race_record.h"
#include "kinect_nui_record.h"
#include "avatar_state.h"
#include "nui_analysis_trace.h"
#include "motion_trace.h"
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <sstream>
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
// A real Kinect's players are read by the title's own depth detectors, as on
// the console: its lean, crouch, jump and side results always stand, and
// nothing here stands in for them. This only notes, with SFR_KINECT_RECORD,
// what the detectors answered (sensor_row).
bool sensor_steering = false;
uint32_t sensor_record = 0;
void write_camera_lean(uint32_t record,float scale=1.f,const sfr::CameraRaceMotion& motion=camera_motion) {
    // These are depth-image pixel counts in the original game, not joints.
    // RGB pose input supplies their ratio; zero/zero means full right lean.
    const float lean=motion.lean()*scale;
    store_float(record+640,1.f+std::max(lean,0.f));
    store_float(record+644,1.f+std::max(-lean,0.f));
}
struct SensorRow {
    uint64_t epoch=0;
    uint32_t tracking_id=0;
    // SFR_KINECT_RECORD (kinect_race_record.h): this update's row, written
    // once the update's detectors have answered (the next update, or when
    // the record is dropped).
    sfr::KinectRaceRecord row;
    bool pending=false;
};
std::unordered_map<uint32_t,SensorRow> sensor_rows;
uint64_t sensor_epoch=0;
uint32_t sensor_manager=0;
void flush_race_row(SensorRow& state) {
    if(!state.pending)return;
    state.pending=false;
    if(sfr::KinectRecordFile* file=sfr::kinect_record_race()) {
        std::string text;
        sfr::kinect_race_record_row(state.row,text);
        file->write(std::move(text));  // queued: never waits for the disk
    }
}
void flush_race_rows() {
    for(auto& item:sensor_rows)flush_race_row(item.second);
}
void update_motion(uint32_t record,uint64_t generation,sfr::CameraRaceMotion& motion,uint64_t& last_tick) {
    const uint64_t now=sfr::camera_motion_clock_ns();
    const float seconds=last_tick && now>last_tick?float(double(now-last_tick)*1e-9):1.f/60.f;
    last_tick=now;
    const auto point=[&](uint32_t offset) {
        return std::array{load_float(record+offset),load_float(record+offset+4),load_float(record+offset+8)};
    };
    const bool ready=motion.ready();
    sfr::CameraRacePose pose{point(0),point(32),point(224),point(288),point(64),point(128),point(192),point(256),point(80),point(96),point(144),point(160),point(208),point(272)};
    motion.observe(pose,seconds,generation);
    if(motion.ready()!=ready)
        std::cerr<<"CAMERA_RACE_CALIBRATION ready="<<motion.ready()<<'\n';
}

void update_camera_motion(uint32_t record,uint64_t generation) {
    update_motion(record,generation,camera_motion,camera_last_tick);
}
// The row of one body record's update, filled once per update (the title
// asks several detectors about it). Only while recording.
SensorRow* sensor_row(uint32_t record) {
    if(!sfr::kinect_record_race())return nullptr;
    auto& state=sensor_rows[record];
    // Native race records retain a pointer to their NUI_SKELETON_DATA at
    // +768 (82438A98). Its state/id detect a new person reusing the allocation.
    const uint32_t data=memory().readable(uint64_t(record)+768,4)?memory().load<uint32_t>(uint64_t(record)+768):0;
    const bool tracked=data && memory().readable(data,8) && memory().load<uint32_t>(data)==2;
    const uint32_t id=tracked?memory().load<uint32_t>(uint64_t(data)+4):0;
    if(!id || state.tracking_id!=id) {flush_race_row(state);state={};state.tracking_id=id;}
    if(!id)return nullptr;
    if(state.epoch!=sensor_epoch) {
        flush_race_row(state);
        state.row=sfr::KinectRaceRecord{};
        state.row.steady_us=sfr::kinect_record_now_us();
        state.row.generation=sfr::kinect_frame_generation();
        state.row.record=record;
        state.row.tracking_id=id;
        // The title's own depth view's lean pair, as it computed it.
        state.row.depth_right=load_float(record+640);
        state.row.depth_left=load_float(record+644);
        // Everything the depth view wrote for this record from its newest
        // image, read on the view's own thread right after it ran (so all of
        // one image, and the image's number with it: nui_hooks.cpp).
        if(!sfr::kinect_depth_view_latest(record,state.row.view) && memory().readable(uint64_t(record)+784,4)) {
            // Published by slot too: a record's +784 is its skeleton slot (82438A98).
            const uint32_t slot=memory().load<uint32_t>(uint64_t(record)+784);
            if(slot<6) sfr::kinect_depth_view_latest(slot,state.row.view);
        }
        state.pending=true;
        state.epoch=sensor_epoch;
    }
    return &state;
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
std::optional<uint32_t> camera_gesture(uint32_t address,uint32_t detector,uint32_t results,const sfr::CameraRaceMotion& motion=camera_motion) {
    const auto primary=[&](uint32_t bits){set_bits(entry(results)+4,bits);};
    switch(address) {
    case 0x822C9050:
        memory().store<uint32_t>(detector+88,motion.jump()?0xFFFFFFFFu:1u);
        memory().store<uint32_t>(detector+92,motion.jump()?0xFFFFFFFFu:1u);
        if(motion.jump()){primary(0x200);return 1;}
        return 2;
    case 0x822C8778: case 0x822CB840:
        if(motion.crouch()){primary(0x7000);return 1;}
        return 2;
    case 0x822C9A80: case 0x822CAF48:
        if(motion.boost()){primary(0x8000);return 1;}
        return 0;
    case 0x822CA6B0:
        if(!motion.brake_ready()){set_bits(entry(results)+20,0x2000);return 2;}
        break;
    case 0x822C9BF0:
        if(!motion.brake_ready()){set_byte(entry(results)+80,0);store_float(detector+72,0);return 2;}
        break;
    case 0x822CA518:
        if(motion.jump_protected()||(motion.arm_action()&&!motion.kick_leg_action())) {
            set_byte(detector+72,0);return 2;
        }
        break;
    case 0x822CBD28:
        if(motion.jump_protected()||(motion.arm_action()&&!motion.kick_leg_action())) {
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
thread_local bool live_controller_lean = false;
thread_local uint32_t live_steering_filter = 0;
struct SteeringFilterScope {
    uint32_t previous = live_steering_filter;
    explicit SteeringFilterScope(uint32_t filter) { live_steering_filter = filter; }
    ~SteeringFilterScope() { live_steering_filter = previous; }
};
struct RaceSourceScope {
    uint32_t previous = live_race_source;
    bool previous_controller_lean = live_controller_lean;
    explicit RaceSourceScope(uint32_t source) { live_race_source = source; live_controller_lean = false; }
    ~RaceSourceScope() { live_race_source = previous; live_controller_lean = previous_controller_lean; }
};

uint32_t input_player(uint32_t slot, bool multiple) {
    if (!multiple) return 0;
    // SFR_RACE_TWO_PLAYERS=0 puts both riders back on the first pad, which
    // is what they shared before any of this.
    static const bool split = [] { const char* t = std::getenv("SFR_RACE_TWO_PLAYERS");
                                   return !t || !*t || *t != '0'; }();
    if (!split) return 0;
    static const bool swap = [] { const char* t = std::getenv("SFR_RACE_PLAYERS_SWAP");
                                  return t && *t && *t != '0'; }();
    return swap ? 1u - slot : slot;
}

uint32_t player_of_record(uint32_t record) {
    // One player's race is asked about through two objects of their own.
    const uint32_t slot = record && record != body_address && record != original_body ? 1u : 0u;
    return input_player(slot, two_players);
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

PPC_FUNC_IMPL(__imp__sub_822A6988);

SFR_INPUT_HOOK(sub_822A6988) {
    sfr::enter_function(ctx, "sub_822A6988", 0x822A6988);
    // 822A5140 interprets controller-overwritten body fields as joint XYZ.
    // Keep authored animation and gesture handling; omit only its seven
    // tracked-body rotation overlays for the current local controller rider.
    const auto tracked_rotation = [&] {
        switch (uint32_t(ctx.lr)) {
        case 0x822A46E0: return ctx.r4.u32 == 2;
        case 0x822A46F4: return ctx.r4.u32 == 5;
        case 0x822A4708: return ctx.r4.u32 == 7;
        case 0x822A471C: return ctx.r4.u32 == 9;
        case 0x822A4730: return ctx.r4.u32 == 6;
        case 0x822A4744: return ctx.r4.u32 == 8;
        case 0x822A4758: return ctx.r4.u32 == 10;
        default: return false;
        }
    };
    if (pad_racing() && !sfr::nui_body_from_sensor() && tracked_rotation()) {
        const auto slot = sfr::local_rider_slot(memory(), ctx.r3.u32);
        if (slot && !camera_player(input_player(*slot, two_players)))
            return;
    }
    __imp__sub_822A6988(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82438930);

// Kinect manager update, once per frame before it handles the players: in a
// race ([83E52F8C] nonzero) P1's body record (KinnectNuiBox+0x78) is ours,
// refreshed from the pad; afterwards the title's record is put back.
SFR_INPUT_HOOK(sub_82438930) {
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
    const uint32_t current_sensor_record=sensor_steering?m.load<uint32_t>(box+0x78):0;
    if(!sensor_steering || sensor_manager!=box || sensor_record!=current_sensor_record) {
        flush_race_rows();
        sensor_rows.clear();
    }
    sensor_record=current_sensor_record;
    sensor_manager=sensor_steering?box:0;
    ++sensor_epoch;
    // Retire absent records without ever following their saved guest pointers.
    std::erase_if(sensor_rows,[](auto& item){
        const bool old=sensor_epoch-item.second.epoch>30;
        if(old)flush_race_row(item.second);
        return old;
    });
    if(sensor_steering!=was_steering)
        std::cerr<<"NUI_RACE_SENSOR active="<<sensor_steering<<" record=0x"<<std::hex<<sensor_record<<std::dec<<'\n';
    static const bool motion_trace=sfr::motion_trace_enabled();
    const auto work_began=std::chrono::steady_clock::now();
    if(racing && box && original_body && camera_player(0))update_camera_motion(original_body,sfr::camera_pose_generation());
    else {camera_motion.reset();camera_last_tick=0;}
    if(motion_trace && sensor_steering) {
        // The skeleton fallback's own share of a race frame (KINECT_COST
        // part=race), every 300 updates: it should be microseconds.
        static sfr::TraceCost cost;
        cost.add(std::chrono::steady_clock::now()-work_began);
        if(cost.calls()>=300) {
            std::cerr<<"KINECT_COST part=race calls="<<cost.calls()<<" mean_us="<<cost.mean_us()<<" max_us="<<cost.max_us()<<'\n';
            cost.clear();
        }
    }
    static uint32_t motion_frames=0;
    if(motion_trace && racing && (++motion_frames%30==0 || camera_motion.jump() || camera_motion.overthrow())) {
        std::cerr<<"CAMERA_RACE_MOTION title_step="<<seconds<<" generation="<<sfr::camera_pose_generation()
                 <<" camera="<<camera_player(0)<<" ready="<<camera_motion.ready()
                 <<" lean="<<camera_motion.lean()
                 <<" crouch="<<camera_motion.crouch()
                 <<" jump="<<camera_motion.jump()<<" acceleration="<<camera_motion.boost()<<" arm_action="<<camera_motion.arm_action()
                 <<" overthrow="<<camera_motion.overthrow()<<" kick_leg="<<camera_motion.kick_leg_action()<<'\n';
    }
}

// SFR_KINECT_RECORD's <prefix>-nui.csv (kinect_nui_record.h): what every
// detector the study lists answered for a real Kinect's player, gathered
// here and written with the player's next input update (nui_hooks.cpp).
// The player is the one whose body record (box +120/+124, or the input
// block's +12) the detector was asked about.
int nui_player_of_record(uint32_t record) {
    auto& m = memory();
    const auto word = [&](uint64_t at) { return m.readable(at, 4) ? m.load<uint32_t>(at) : 0u; };
    if (!record) return -1;
    if (const uint32_t box = word(nui_box))
        for (uint32_t p = 0; p < 2; ++p)
            if (word(uint64_t(box) + 120 + 4 * p) == record) return int(p);
    if (const uint32_t input = word(0x83E52FB8))
        for (uint32_t p = 0; p < 2; ++p)
            if (word(uint64_t(input) + 32 + 2544 * p + 12) == record) return int(p);
    return -1;
}
bool nui_recording() { return sensor_steering && sfr::kinect_record_nui() != nullptr; }
// The result entry a detector writes (front curve: the second one), and
// its +4/+20 words before the detector ran, so that its own bits are known.
struct NuiEntryBits {
    uint32_t entry = 0, primary = 0, secondary = 0;
};
NuiEntryBits nui_entry_before(uint32_t address, uint32_t results) {
    NuiEntryBits bits;
    if (!results || !memory().readable(results, 16)) return bits;
    bits.entry = entry(results, address == 0x822CD3F0 ? 12 : 8);
    if (!memory().readable(bits.entry, 84)) { bits.entry = 0; return bits; }
    bits.primary = memory().load<uint32_t>(bits.entry + 4);
    bits.secondary = memory().load<uint32_t>(bits.entry + 20);
    return bits;
}
void note_nui_detector(int player, uint32_t address, uint32_t detector, const NuiEntryBits& before, uint32_t result) {
    const int index = sfr::kinect_nui_detector_index(address);
    if (player < 0 || index < 0 || !before.entry) return;
    auto& m = memory();
    const uint32_t e = before.entry;
    const uint32_t primary = m.load<uint32_t>(e + 4) & ~before.primary;
    const uint32_t secondary = m.load<uint32_t>(e + 20) & ~before.secondary;
    namespace v = sfr::kinect_nui_value_kind;
    bool has_value = true;
    float value = 0;
    switch (sfr::kinect_nui_detectors[size_t(index)].value) {
    case v::entry_byte_80: value = float(m.load<uint8_t>(e + 80)); break;
    case v::entry_word_72: value = float(m.load<uint32_t>(e + 72)); break;
    case v::entry_float_56: value = load_float(e + 56); break;
    case v::entry_signed_82: value = float(int8_t(m.load<uint8_t>(e + 82))); break;
    case v::entry_signed_83: value = float(int8_t(m.load<uint8_t>(e + 83))); break;
    case v::second_entry_signed_81: value = float(int8_t(m.load<uint8_t>(e + 81))); break;
    case v::entry_signed_76: value = float(int8_t(m.load<uint8_t>(e + 76))); break;
    case v::detector_short_76:
        has_value = detector && m.readable(uint64_t(detector) + 76, 4);
        if (has_value) {
            value = float(int16_t(m.load<uint16_t>(uint64_t(detector) + 76)));
            sfr::kinect_nui_recorder().wipe_y(uint32_t(player), float(int16_t(m.load<uint16_t>(uint64_t(detector) + 78))));
        }
        break;
    default: has_value = false; break;
    }
    sfr::kinect_nui_recorder().detector(uint32_t(player), address, int(result), primary, secondary, has_value, value);
}

PPC_FUNC_IMPL(__imp__sub_822C6200);
PPC_FUNC_IMPL(__imp__sub_82918418);

namespace {
// SFR_NUI_ANALYSIS_TRACE=1 (nui_analysis_trace.h), while racing: how the
// title turns, five lines a second of each stage.
// - stage=lean, after 822C6200(source): its two ten-sample filters
//   (822C63E8: newest sample first, the weighted mean at +40 of each), the
//   depth view's lean (record +640/+644 as a ratio, at most +-3.5) at
//   source +4 with its mean at source +44, and the skeleton's tilt (the
//   angle of head - spine in the XY plane, signed) at source +48 with its
//   mean at source +88.
// - stage=steer, after 822C8958(detector, source, results): the lean it read
//   (source +44), its own filter's mean (detector +72, mean at +112), the
//   steering it wrote (result entry +76 and +78, signed bytes), the
//   direction bits (+4 & 3) and its answer.
uint32_t trace_record_of(uint32_t source) {
    auto& m = memory();
    const auto word = [&](uint64_t at) { return m.readable(at, 4) ? m.load<uint32_t>(at) : 0u; };
    const uint32_t object = source ? word(source) : 0, vtable = object ? word(object) : 0;
    return vtable && word(uint64_t(vtable) + 4) == 0x82918418 ? word(uint64_t(object) + 4) : 0;
}
void trace_turn_samples(std::ostream& line, const char* name, uint32_t filter) {
    line << ' ' << name << '=';
    for (uint32_t i = 0; i < 10; ++i) line << (i ? "," : "") << load_float(filter + 4 * i);
    line << ' ' << name << "_mean=" << load_float(filter + 40);
}
void trace_lean(uint32_t source) {
    if (!sfr::nui_analysis_trace() || !source || !memory().load<uint32_t>(race_flag)) return;
    static sfr::NuiTraceLimiter limiter("TURN", 5.0, 18000);
    if (!memory().readable(source, 92) || !limiter.take()) return;
    const uint32_t record = trace_record_of(source);
    const uint32_t data = record && memory().readable(uint64_t(record) + 768, 4) ? memory().load<uint32_t>(record + 768) : 0;
    std::ostringstream line;
    line << "NUI_TRACE_TURN stage=lean source=0x" << std::hex << source << " record=0x" << record << std::dec
         << " tracking_id=" << (data && memory().readable(uint64_t(data) + 4, 4) ? memory().load<uint32_t>(data + 4) : 0);
    if (record && memory().readable(uint64_t(record) + 640, 8))
        line << " depth_right=" << load_float(record + 640) << " depth_left=" << load_float(record + 644);
    trace_turn_samples(line, "depth_lean", source + 4);
    trace_turn_samples(line, "tilt", source + 48);
    std::cerr << line.str() << '\n';
}
void trace_steer(uint32_t detector, uint32_t source, uint32_t results, uint32_t result) {
    if (!sfr::nui_analysis_trace() || !detector || !source || !results || !memory().load<uint32_t>(race_flag)) return;
    static sfr::NuiTraceLimiter limiter("TURN", 5.0, 18000);
    if (!memory().readable(detector, 116) || !memory().readable(source, 48) || !limiter.take()) return;
    const uint32_t e = entry(results);
    std::ostringstream line;
    line << "NUI_TRACE_TURN stage=steer detector=0x" << std::hex << detector << " source=0x" << source << " record=0x"
         << trace_record_of(source) << std::dec << " lean_in=" << load_float(source + 44)
         << " filter_mean=" << load_float(detector + 112);
    if (memory().readable(e, 84))
        line << " steer=" << int(int8_t(memory().load<uint8_t>(e + 76))) << " steer78="
             << int(int8_t(memory().load<uint8_t>(e + 78))) << " bits=" << (memory().load<uint32_t>(e + 4) & 3u);
    line << " result=" << result;
    std::cerr << line.str() << '\n';
}
}

// This original race consumer updates source->object through vtable[2],
// then reads its body through vtable[1] before calculating lean. Keep that
// order and the title's lean calculation. Controller lean bypasses only the
// depth-input smoothing below, after the live reader identifies its player.
SFR_INPUT_HOOK(sub_822C6200) {
    sfr::enter_function(ctx, "sub_822C6200", 0x822C6200);
    const uint32_t source = ctx.r3.u32;
    {
        RaceSourceScope scope(pad_racing() ? ctx.r3.u32 : 0);
        __imp__sub_822C6200(ctx, base);
    }
    trace_lean(source);
    // SFR_KINECT_RECORD: the means of its two filters, for the nui.csv row.
    if (!pad_racing() && nui_recording() && memory().readable(source, 92)) {
        const int player = nui_player_of_record(body_of_source(ctx, base, source));
        if (player >= 0) sfr::kinect_nui_recorder().lean(uint32_t(player), load_float(source + 44), load_float(source + 88));
    }
}

// 82918418 is a shared accessor, so calls outside the live race source's
// update retain their original behavior. The scope restores on exceptions
// and is thread-local because guest consumers can run on different threads.
SFR_INPUT_HOOK(sub_82918418) {
    sfr::enter_function(ctx, "sub_82918418", 0x82918418);
    const uint32_t object = ctx.r3.u32;
    __imp__sub_82918418(ctx, base);
    const uint32_t record = ctx.r3.u32;
    if (pad_racing() && live_race_source && record &&
        memory().load<uint32_t>(live_race_source) == object) {
        const uint32_t player=player_of_record(record);
        if(camera_player(player))write_camera_lean(record);
        else {
            race[player].write(memory(), record);
            live_controller_lean = true;
        }
    }
}

PPC_FUNC_IMPL(__imp__sub_822C63E8);
SFR_INPUT_HOOK(sub_822C63E8) {
    sfr::enter_function(ctx, "sub_822C63E8", 0x822C63E8);
    const uint32_t filter = ctx.r3.u32;
    const float input = float(ctx.f1.f64);
    const bool direct = (live_controller_lean && live_race_source && filter == live_race_source + 4) ||
                        (live_steering_filter && filter == live_steering_filter);
    // The original keeps ten samples for noisy depth input. At low FPS that
    // delays controller reversal/release by much longer than on the console.
    // Keep its history (including across input handoff), but use this update's
    // already-clamped analog lean for pads. The adjacent body-angle filter,
    // unrelated callers, Camera and physical Kinect retain original results.
    __imp__sub_822C63E8(ctx, base);
    if (direct) {
        static const bool trace = [] { const char* p = std::getenv("SFR_RACE_LEAN_TRACE"); return p && *p == '1'; }();
        static thread_local unsigned samples = 0;
        if (trace && samples < 120 && std::fabs(input - float(ctx.f1.f64)) > .0001f) {
            ++samples;
            std::cerr << "NUI_RACE_LEAN filter=0x" << std::hex << filter << std::dec
                      << " input=" << input << " smoothed=" << ctx.f1.f64 << " applied=" << input << '\n';
        }
        store_float(filter + 40, input);
        ctx.f1.f64 = input;
    }
}

// Opt-in detector transitions: numeric state only, no camera images.
static void trace_camera_detector(uint32_t address,uint32_t detector,uint32_t results,uint32_t result) {
    if(address!=0x822C8778 && address!=0x822CB840 && address!=0x822CA6B0 && address!=0x822C9BF0 && address!=0x822C9050 &&
       address!=0x822CA518 && address!=0x822CBD28 && address!=0x822CC590 && address!=0x822CCD98 && address!=0x822CD068)return;
    static const bool enabled=sfr::motion_trace_enabled();
    if(!enabled)return;
    static std::unordered_map<uint64_t,std::array<uint32_t,4>> last;
    static sfr::TraceRate rate(20);
    const uint64_t key=uint64_t(address)<<32|detector;
    const uint32_t left_arm=address==0x822CC590?memory().load<uint32_t>(detector+72):0;
    const uint32_t right_arm=address==0x822CC590?memory().load<uint32_t>(detector+76):0;
    const uint32_t kick_state=address==0x822CA518?memory().load<uint8_t>(detector+72):
        address==0x822CBD28?memory().load<uint32_t>(detector+72):0;
    const std::array state{result,left_arm,right_arm,kick_state};
    const bool changed=!last.contains(key)||last[key]!=state;
    last[key]=state;
    if((changed||camera_motion.jump()) && rate.allow())std::cerr<<"CAMERA_GESTURE address=0x"<<std::hex<<address
        <<" detector=0x"<<detector<<" primary=0x"<<memory().load<uint32_t>(entry(results)+4)
        <<" secondary=0x"<<memory().load<uint32_t>(entry(results)+20)<<std::dec
        <<" arm_first="<<left_arm<<" arm_second="<<right_arm<<" result="<<result<<" generation="<<sfr::camera_pose_generation()<<" motion_jump="<<camera_motion.jump()<<" brake_ready="<<camera_motion.brake_ready()<<" arm_action="<<camera_motion.arm_action()
        <<" kick_state="<<kick_state<<" kick_leg="<<camera_motion.kick_leg_action()<<" dropped="<<rate.dropped()<<'\n';
}

// Opt-in numeric consumer trace: distinguish recognized crouch from title
// restrictions (for example the opening tutorial masks other race actions).
PPC_FUNC_IMPL(__imp__sub_822B60F8);
SFR_INPUT_HOOK(sub_822B60F8) {
    sfr::enter_function(ctx,"sub_822B60F8",0x822B60F8);
    const uint32_t actor=ctx.r3.u32;
    __imp__sub_822B60F8(ctx,base);
    // A webcam player's race: did the rider take the crouch and jump it was
    // given? (A real Kinect's players are read by the title's own detectors.)
    static const bool enabled=sfr::motion_trace_enabled();
    if(!enabled)return;
    const bool camera=pad_racing() && sfr::camera_motion_active();
    if(!camera)return;
    const sfr::CameraRaceMotion& motion=camera_motion;
    const uint32_t owner=memory().load<uint32_t>(actor+1680);
    if(!owner)return;
    const uint32_t results=memory().load<uint32_t>(owner+3220);
    const uint32_t animation=memory().load<uint32_t>(owner+3208),state=memory().load<uint32_t>(owner+3212);
    if(!results || !animation || !state)return;
    const uint32_t entries=memory().load<uint32_t>(results+16),data=memory().load<uint32_t>(animation+12);
    if(!entries || !data)return;
    const uint32_t read=entries+84*memory().load<uint32_t>(results+28);
    const std::array<uint32_t,6> now{memory().load<uint32_t>(read+4),memory().load<uint32_t>(read+8),
        memory().load<uint32_t>(data+44),memory().load<uint32_t>(state+400),uint32_t(motion.crouch()),uint32_t(motion.jump())};
    static std::unordered_map<uint32_t,std::array<uint32_t,6>> previous;
    if(previous.contains(actor) && previous.at(actor)==now)return;
    previous.insert_or_assign(actor,now);
    static sfr::TraceRate rate(30);
    if(!rate.allow())return;
    std::cerr<<"CAMERA_RACE_ACTOR actor=0x"<<std::hex<<actor<<" ring=0x"<<results+16
             <<" written=0x"<<memory().load<uint32_t>(entries+84*memory().load<uint32_t>(results+24)+4)
             <<" flags=0x"<<now[0]<<" released=0x"<<now[1]
             <<" mask1=0x"<<memory().load<uint32_t>(results+36)<<" mask2=0x"<<memory().load<uint32_t>(results+40)<<std::dec
             <<" anim="<<now[2]<<" state="<<now[3]<<" crouch="<<now[4]<<" jump="<<now[5]
             <<" enabled="<<unsigned(memory().load<uint8_t>(actor+1989))<<" block="<<load_float(actor+2080)
             <<" block2636="<<unsigned(memory().load<uint8_t>(actor+2636))<<" source="<<(camera?"camera":"kinect")
             <<" dropped="<<rate.dropped()<<'\n';
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

// A real Kinect's detectors answer from the title's own depth view and the
// answer always stands. With SFR_KINECT_RECORD only, the answer is noted in
// the update's row (original is what the title said, result what the race
// used: the same).
void finish_sensor_detector(PPCContext& ctx,uint8_t* base,uint32_t address,uint32_t source) {
    if(address!=0x822C8778 && address!=0x822CB840 && address!=0x822C9050 && address!=0x822CA6B0)return;
    if(!sfr::kinect_record_race())return;
    const int column=sfr::kinect_race_detector_index(address);
    if(column<0)return;
    const uint32_t record=body_of_source(ctx,base,source);
    if(!record)return;
    if(SensorRow* found=sensor_row(record); found && found->pending) {
        found->row.original[size_t(column)]=int(ctx.r3.u32);
        found->row.result[size_t(column)]=int(ctx.r3.u32);
    }
}

// One detector override: while the pad drives the race the body decides,
// otherwise the original detector runs.
#define RACE_DETECTOR(address, ...)                                      \
    PPC_FUNC_IMPL(__imp__sub_##address);                                       \
    SFR_INPUT_HOOK(sub_##address) {                                            \
        sfr::enter_function(ctx, "sub_" #address, 0x##address);               \
        if (!pad_racing()) {                                                   \
            const uint32_t source = ctx.r4.u32;                                 \
            const uint32_t nui_detector = ctx.r3.u32;                           \
            const bool nui_wanted = nui_recording();                            \
            const NuiEntryBits nui_before = nui_wanted ? nui_entry_before(0x##address, ctx.r5.u32) : NuiEntryBits{}; \
            __imp__sub_##address(ctx, base);                                   \
            if (sensor_steering)                                               \
                finish_sensor_detector(ctx, base, 0x##address, source);        \
            if (nui_wanted)                                                    \
                note_nui_detector(nui_player_of_record(body_of_source(ctx, base, source)), 0x##address, \
                                  nui_detector, nui_before, ctx.r3.u32);       \
            return;                                                            \
        }                                                                      \
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

// CatchLR / Catch: the legacy pad fields overlap left wrist X and elbow Z.
// Use the intended right-stick reach directly rather than feeding those
// button values into the original skeleton-angle tests. Stance is handled
// by the title's consumers of these left/right flags, not by the detector.
#define RACE_REACH(address)                                                   \
    RACE_DETECTOR(address, {                                                 \
        result = 2;                                                         \
        if (b.right_y <= 0 && std::fabs(b.right_x) > .3f) {                 \
            const bool left = b.right_x < 0;                                \
            set_bits(entry(results) + 4, left ? 0x20000000u : 0x40000000u); \
            set_bits(entry(results) + 20, left ? 0x20000u : 0x40000u);       \
            result = 1;                                                     \
        }                                                                   \
    })
RACE_REACH(822C9E90)
RACE_REACH(822CB340)

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

// Tricks: turning the left stick around turns the trick. As the original
// detector does with the body's turn angle (body+772, degrees, valid when
// body+788 is set): the detector keeps the last angle at +76 and whether it
// had one at +80, adds each frame's turn to its total at +72, and reports
// that total in degrees at +56 of the result, with +4 bit 0x800, on every
// call. (Half turns at +56, as this used to write, are 180 times too small:
// no amount of turning rated a jump.)
RACE_DETECTOR(822C9938, {
    const bool turning = b.stick_x * b.stick_x + b.stick_y * b.stick_y >= 0.25f;
    float total = load_float(detector + 72);
    if (turning) {
        const float angle = std::atan2(b.stick_y, b.stick_x) * 57.29578f;
        if (memory().load<uint8_t>(detector + 80)) {
            float delta = angle - load_float(detector + 76);
            if (delta > 180.0f) delta -= 360.0f;
            else if (delta < -180.0f) delta += 360.0f;
            total += delta;
            store_float(detector + 72, total);
        }
        store_float(detector + 76, angle);
    }
    set_byte(detector + 80, turning ? 1 : 0);
    const uint32_t e = entry(results);
    store_float(e + 56, total);
    set_bits(e + 4, 0x800);
    result = 1;
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

// Holding hands (Tag Race): both players holding LT, as in the No Kinect
// Patch (Issue #109). The original asks about the team, not one source: the
// first two racers of [83E52FDC] (+36 begin, +40 end), each with racer +108
// at 0 or 3, and one's hand within reach of the other's (+20 bit 0x200 of
// the result, the answer kept at +76).
PPC_FUNC_IMPL(__imp__sub_822C8D88);
SFR_INPUT_HOOK(sub_822C8D88) {
    sfr::enter_function(ctx, "sub_822C8D88", 0x822C8D88);
    if (!pad_racing() || !two_players || camera_player(0) || camera_player(1)) {
        // The team's answer, recorded with the first player (it asks about
        // two racers, not one source).
        const uint32_t detector = ctx.r3.u32;
        const bool nui_wanted = !pad_racing() && nui_recording();
        const NuiEntryBits nui_before = nui_wanted ? nui_entry_before(0x822C8D88, ctx.r5.u32) : NuiEntryBits{};
        __imp__sub_822C8D88(ctx, base);
        if (nui_wanted) note_nui_detector(0, 0x822C8D88, detector, nui_before, ctx.r3.u32);
        return;
    }
    const uint32_t detector = ctx.r3.u32, results = ctx.r5.u32;
    const auto racer_ready = [&](uint32_t index) {
        const uint32_t team = memory().load<uint32_t>(0x83E52FDC);
        if (!team || int32_t(memory().load<uint32_t>(team + 20)) <= int32_t(index)) return false;
        const uint32_t begin = memory().load<uint32_t>(team + 36), end = memory().load<uint32_t>(team + 40);
        if (end <= begin || (end - begin) / 4 <= index) return false;
        const uint32_t racer = memory().load<uint32_t>(begin + index * 4);
        if (!racer) return false;
        const uint32_t state = memory().load<uint32_t>(racer + 108);
        return state == 0 || state == 3;
    };
    uint32_t result = 2;
    if (racer_ready(0) && racer_ready(1) && race[input_player(0, true)].body().left_trigger &&
        race[input_player(1, true)].body().left_trigger) {
        set_bits(entry(results) + 20, 0x200);
        result = 1;
    }
    {
        static uint32_t last = 0;
        if (result != last) {
            last = result;
            std::cerr << "NUI_TAG_HANDS held=" << (result == 1) << '\n';
        }
    }
    memory().store<uint32_t>(detector + 76, result);
    ctx.r3.u64 = result;
}

// The detector at 821A2484 (original 822CB9B8) uses the one at 821A2344
// (822C8958) while the pad drives the race.
PPC_FUNC_IMPL(__imp__sub_822C8958);
SFR_INPUT_HOOK(sub_822C8958) {
    sfr::enter_function(ctx, "sub_822C8958", 0x822C8958);
    const bool controller = pad_racing() && !camera_player(player_of_source(ctx, base, ctx.r4.u32));
    // After reading source+44, the title maps lean to steering strength and
    // filters it again at detector+72 before writing result bytes +76/+78.
    // Retain that gear/stance mapping, bypass only its controller smoothing.
    const uint32_t detector = ctx.r3.u32, source = ctx.r4.u32, results = ctx.r5.u32;
    const bool nui_wanted = !pad_racing() && nui_recording();
    const NuiEntryBits nui_before = nui_wanted ? nui_entry_before(0x822C8958, results) : NuiEntryBits{};
    {
        SteeringFilterScope scope(controller ? ctx.r3.u32 + 72 : 0);
        __imp__sub_822C8958(ctx, base);
    }
    trace_steer(detector, source, results, ctx.r3.u32);
    if (nui_wanted)
        note_nui_detector(nui_player_of_record(body_of_source(ctx, base, source)), 0x822C8958, detector, nui_before, ctx.r3.u32);
}
PPC_FUNC_IMPL(__imp__sub_822CB9B8);
SFR_INPUT_HOOK(sub_822CB9B8) {
    sfr::enter_function(ctx, "sub_822CB9B8", 0x822CB9B8);
    if (pad_racing() && !camera_player(player_of_source(ctx,base,ctx.r4.u32))) sub_822C8958(ctx, base);
    else {
        const uint32_t detector = ctx.r3.u32, source = ctx.r4.u32;
        const bool nui_wanted = !pad_racing() && nui_recording();
        const NuiEntryBits nui_before = nui_wanted ? nui_entry_before(0x822CB9B8, ctx.r5.u32) : NuiEntryBits{};
        __imp__sub_822CB9B8(ctx, base);
        if (nui_wanted)
            note_nui_detector(nui_player_of_record(body_of_source(ctx, base, source)), 0x822CB9B8, detector, nui_before,
                              ctx.r3.u32);
    }
}

PPC_FUNC_IMPL(__imp__sub_822B72E0);

// Race preparation ("On your Gear!"), state at +2408: 0 starts it, 1 waits
// for the sensor, 2..6 run the body measurements of the chosen gear (each
// step is a pose the title reads from the skeleton), 7 ends the preparation
// and 8 races. A pad has no body to measure, so its preparation ends with
// the measurements skipped.
SFR_INPUT_HOOK(sub_822B72E0) {
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
