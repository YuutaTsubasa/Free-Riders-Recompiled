#include "diagnostic_hooks.h"
#include "draw_read_watch.h"
#include "ppc_recomp_shared.h"
#include "race_frame_clock.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>
#include <unordered_map>
#include <vector>

// The title's task manager (824A0458) runs each frame's work as phases over
// task objects (824A00C8). The race's logic is in two of them, each task's
// methods 4 (handed to the job workers by 8249F048) and 8, which make no
// graphics calls; its drawing is method 12 (run through 822980C8 in phases 4
// and 8), which made about 13,600 graphics calls and took about 10 of a race
// frame's 11 ms on a PC. A race frame the frame skipper does not draw
// (race_frame_clock.h) leaves most of that drawing out, so it costs little
// more than the race itself, and the race keeps real time on a slow device.
namespace sfr { bool race_frame_drawn(); }

namespace {
constexpr uint32_t race_flag = 0x83E52F8C;
constexpr uint32_t draw_method = 0x822980C8;  // calls a task's method 12

// The task kinds (by vtable) whose drawing a frame that is not drawn leaves
// out: nearly all of a race frame's drawing (about 12 of 12.4 ms on a PC).
// Nothing outside drawing reads what their drawing writes: a read watch over
// the task objects and every page the main thread wrote while drawing
// (SFR_TEST_DRAW_READS below) found readers only of the player racer's
// matrix (821A1920), of item-like objects' positions (821A50xx, 821A4F70),
// of the drawing's own allocator and of the guest's rendering thread. Kinds
// not listed, those among them, always draw.
// 8219F910 also always draws: with its drawing left out, a racer launched
// off Dolphin Resort's big jump fell short of the far side, again and again,
// on an AYN Thor skipping two frames in three (drawn frames made 20 ms
// slower). Found with SFR_TEST_DRAW_KEEP below; the read watch's races had
// not passed that jump. Drawing it costs little (race speed 0.996 there).
// 821A1968 and 821A61F4 also always draw: with both left out, the racer fell
// out of Metropolis Speedway into the void about a minute into the race, in
// both of two runs on the Thor set up as above (issue #154: through the
// train, railings that could not be grabbed). With either one drawing it did
// not fall, so both draw.
constexpr uint32_t undrawn_skips[] = {
    0x821A8A78, 0x821A8998, 0x821A319C, 0x821A25A4,
    0x821A3874, 0x821A2D70, 0x821AB4AC, 0x821A2A94, 0x8219F8D0, 0x821A333C, 0x821A38A0};

// SFR_RACE_SKIP_DRAWING=0: frames that are not drawn still run all the
// drawing (as v0.8.3's frame skipping did).
bool skip_drawing_enabled() {
    static const bool on = [] { const char* t = std::getenv("SFR_RACE_SKIP_DRAWING"); return !t || *t != '0'; }();
    return on;
}

// SFR_TEST_DRAW_KEEP=<vtable>[+<vtable>...] (testing, hex): kinds that still
// draw in a frame that is not drawn, to find which one a race needs.
bool kept_when_undrawn(uint32_t vtable) {
    static const std::vector<uint32_t> kept = [] {
        std::vector<uint32_t> kinds;
        if (const char* t = std::getenv("SFR_TEST_DRAW_KEEP"))
            for (char* p = const_cast<char*>(t); *p;) {
                kinds.push_back(uint32_t(std::strtoul(p, &p, 16)));
                if (*p) ++p;
            }
        return kinds;
    }();
    return std::find(kept.begin(), kept.end(), vtable) != kept.end();
}

bool skipped_when_undrawn(uint32_t vtable) {
    return std::find(std::begin(undrawn_skips), std::end(undrawn_skips), vtable) != std::end(undrawn_skips) &&
           !kept_when_undrawn(vtable);
}

// SFR_TEST_DRAW_READS=1 (testing, Windows; races run about ten times
// slower): what a race frame's drawing writes, and who reads it before the
// next frame's drawing, every 600 race frames (DRAW_READS, by the reading
// instruction's offset in the executable, which the link map names, and the
// word's owner: a task kind and offset, or 0xffffffff and the address).
// SFR_TEST_DRAW_FROM=<race frame> starts it later in the race.
namespace watch {
struct Snapshot { uint32_t address, vtable; std::vector<uint8_t> bytes; };
std::vector<Snapshot> snapshots;
std::unordered_map<uint32_t, uint32_t> last_drawn;  // what the drawing last left in a word
std::vector<sfr::draw_reads::Word> words;

bool enabled(uint32_t race_frames) {
    static const uint32_t from = [] { const char* t = std::getenv("SFR_TEST_DRAW_FROM"); return t ? uint32_t(std::atoi(t)) : 0u; }();
    return sfr::draw_reads::enabled() && race_frames >= from;
}
// A word the drawing changed to something other than what it left there the
// frame before; one written the same every frame (a cached pointer, a
// buffer's address) would stay the same if the drawing were skipped.
void changed(uint32_t address, uint32_t vtable, uint32_t offset) {
    const uint32_t value = sfr::active_memory->load<uint32_t>(address);
    const auto last = last_drawn.find(address);
    const bool varies = last != last_drawn.end() && last->second != value;
    last_drawn[address] = value;
    if (varies) words.push_back({address, vtable, offset});
}
void before_drawing(uint8_t* base, const std::vector<uint32_t>& tasks) {
    sfr::draw_reads::end();
    // The heap and the image's globals; not thread stacks (from 0x70000000)
    // nor GPU memory (from 0xA0000000), which the runtime watches itself.
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& range : sfr::active_memory->committed_ranges())
        if (range.first < 0x70000000u || (range.first >= 0x80000000u && range.first < 0xA0000000u)) ranges.push_back(range);
    sfr::draw_reads::begin_capture(base, std::move(ranges), [] { return sfr::current_guest_thread_id(); });
    for (const uint32_t task : tasks)
        snapshots.push_back({task, sfr::active_memory->load<uint32_t>(task), std::vector<uint8_t>(base + task, base + task + 1024)});
}
void after_drawing(uint8_t* base) {
    for (const auto& snap : snapshots)
        for (uint32_t offset = 0; offset < snap.bytes.size(); offset += 4)
            if (std::memcmp(base + snap.address + offset, snap.bytes.data() + offset, 4)) changed(snap.address + offset, snap.vtable, offset);
    snapshots.clear();
    for (const auto& page : sfr::draw_reads::end_capture()) {
        // The graphics device's globals (shader selectors at 82AF6CD8): a
        // read there from a hooked thunk escaped the watch and stopped runs.
        if (page.address == 0x82AF6000u) continue;
        for (uint32_t offset = 0; offset < 4096; offset += 4)
            if (std::memcmp(base + page.address + offset, page.before + offset, 4))
                changed(page.address + offset, 0xFFFFFFFFu, page.address + offset);
    }
    sfr::draw_reads::begin(base, std::exchange(words, {}), [] { return sfr::current_guest_thread_id(); });
}
}
}

PPC_FUNC_IMPL(__imp__sub_824A00C8);
PPC_FUNC(sub_8234FC18);
PPC_FUNC(sub_8249F048);
// 824A00C8 (from 824A0458): [manager+172] is a map of phase nodes walked in
// order with 8234FC18; a node's +12 is its key (1: the phase is handed to
// the job dispatcher 8249F048, unless the byte at manager+192 is set) and
// +16 its phase, whose +0 is the function called on each task of its list
// (+4: sentinel, count), skipping tasks whose flags (+8) meet the phase's
// mask (+12, copied from +8 first). Other keys run on this thread, between
// the manager's callbacks at +196 and +200 (the second says whether to go
// round again). On a race frame that is not drawn, and for the read watch,
// the same is done here, leaving out the drawing of undrawn_skips.
SFR_CONCURRENT_HOOK(sub_824A00C8) {
    auto& memory = *sfr::active_memory;
    static uint32_t race_frames = 0;
    const bool race_frame = sfr::current_guest_thread_id() == 1 && memory.load<uint32_t>(race_flag) != 0;
    if (race_frame) ++race_frames;
    const bool skip_drawing = race_frame && skip_drawing_enabled() && sfr::race_frameskip_enabled() &&
                              !sfr::race_frame_drawn();
    const bool watching = race_frame && watch::enabled(race_frames);
    if (!skip_drawing && !watching) {
        __imp__sub_824A00C8(ctx, base);
        return;
    }
    const uint32_t manager = ctx.r3.u32;
    const uint32_t saved_r1 = ctx.r1.u32;
    ctx.r1.u64 = saved_r1 - 176;  // as the original's frame: the iterator lives there
    const uint32_t iterator = ctx.r1.u32 + 80;
    const auto sentinel = [&] { return memory.load<uint32_t>(memory.load<uint32_t>(manager + 172) + 4); };
    const auto advance = [&] {
        ctx.r3.u64 = iterator;
        sub_8234FC18(ctx, base);
        return memory.load<uint32_t>(iterator);
    };
    memory.store<uint32_t>(iterator, memory.load<uint32_t>(sentinel()));
    for (uint32_t node = memory.load<uint32_t>(iterator); node != sentinel();) {
        const uint32_t phase = memory.load<uint32_t>(node + 16);
        memory.store<uint32_t>(phase + 12, memory.load<uint32_t>(phase + 8));
        node = advance();
    }
    memory.store<uint32_t>(iterator, memory.load<uint32_t>(sentinel()));
    for (uint32_t node = memory.load<uint32_t>(iterator); node != sentinel();) {
        const uint32_t phase = memory.load<uint32_t>(node + 16);
        const uint32_t key = memory.load<uint32_t>(node + 12);
        const uint32_t list = memory.load<uint32_t>(phase + 4);
        const uint32_t function = memory.load<uint32_t>(phase);
        const uint32_t end = memory.load<uint32_t>(list);
        if (watching && function == draw_method && key == 4) {
            std::vector<uint32_t> tasks;
            for (uint32_t element = memory.load<uint32_t>(end); element != end; element = memory.load<uint32_t>(element))
                tasks.push_back(memory.load<uint32_t>(element + 8));
            watch::before_drawing(base, tasks);
        }
        if (memory.load<uint32_t>(list + 4) == 0) {
        } else if (key == 1) {
            if (!memory.load<uint8_t>(manager + 192)) {
                ctx.r3.u64 = manager;
                ctx.r4.u64 = node;
                ctx.r5.u64 = list;
                sub_8249F048(ctx, base);
            }
        } else {
            memory.store<uint32_t>(manager + 204, key);
            for (uint32_t round = 0;; ++round) {
                if (const uint32_t before = memory.load<uint32_t>(manager + 196)) {
                    ctx.r3.u64 = key;
                    ctx.r4.u64 = round;
                    sfr::call_indirect(ctx, base, before);
                }
                for (uint32_t element = memory.load<uint32_t>(end); element != end; element = memory.load<uint32_t>(element)) {
                    const uint32_t task = memory.load<uint32_t>(element + 8);
                    memory.store<uint32_t>(manager + 188, task);
                    const uint32_t mask = memory.load<uint32_t>(phase + 12);
                    if (mask && (memory.load<uint32_t>(task + 8) & mask)) continue;
                    if (skip_drawing && function == draw_method && skipped_when_undrawn(memory.load<uint32_t>(task))) continue;
                    ctx.r3.u64 = task;
                    sfr::call_indirect(ctx, base, function);
                }
                const uint32_t after = memory.load<uint32_t>(manager + 200);
                if (!after) break;
                ctx.r3.u64 = key;
                ctx.r4.u64 = round;
                sfr::call_indirect(ctx, base, after);
                if (!(ctx.r3.u32 & 0xFF)) break;
            }
        }
        if (watching && function == draw_method && key == 8) {
            watch::after_drawing(base);
            if (race_frames % 600 == 0) sfr::draw_reads::report(race_frames);
        }
        node = advance();
    }
    memory.store<uint32_t>(manager + 188, 0);
    ctx.r1.u64 = saved_r1;
}
