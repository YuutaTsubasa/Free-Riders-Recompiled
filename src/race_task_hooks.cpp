#include "diagnostic_hooks.h"
#include "draw_read_watch.h"
#include "ppc_recomp_shared.h"
#include "race_frame_clock.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
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
namespace sfr { bool race_frame_drawn(); extern std::atomic<uint64_t> draw_call_host_ns, draw_call_count; }

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
// not passed that jump. (A stage now runs only the part of its drawing that
// its logic needs: find_stage_areas below.)
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

// A stage's task (8219F910 at Dolphin Resort, 8219F994 at Metropolis
// Speedway; its drawing is 82269638) does one thing besides drawing: first
// it finds which of the course's areas each view's camera is in (82272B48
// into the area list at [83E52FBC]), and the stage's logic (82269468)
// updates only the objects of the areas found (82272C90). Left out, the
// objects of the area a racer flies into stand still, and a racer launched
// off Dolphin Resort's big jump fell short of the far side. In a frame that
// is not drawn the stage now finds its areas and leaves the drawing of its
// objects out (1.0-1.4 ms of a PC's undrawn frame, 2.2-3.1 ms on an AYN
// Thor). A read watch over races at four courses (SFR_TEST_DRAW_READS_KIND
// below) found nothing else of its drawing read outside drawing, but it is
// not enough: on an AYN Thor with frames skipped (threads on one little core)
// the player still fell short of Dolphin Resort's big jump in every race from
// 0.10.25 to 0.10.33, and cleared it with the whole drawing run or with no
// frame skipping. So the whole drawing runs again, as in 0.10.24;
// SFR_UNDRAWN_STAGE_AREAS=1 (a Test option) finds only the areas.
constexpr uint32_t stage_draw = 0x82269638;
bool stage_areas_only() {
    static const bool on = [] { const char* t = std::getenv("SFR_UNDRAWN_STAGE_AREAS"); return t && *t == '1'; }();
    return on;
}
}

PPC_FUNC(sub_828057F0);
PPC_FUNC(sub_828039F8);
PPC_FUNC(sub_82272B48);
PPC_FUNC(sub_822254E8);
namespace {
// 82269638 without the drawing of the stage's objects (its 82269550 loop over
// [task+128..+132] and 82269CD0 over the list at [task+112]).
void find_stage_areas(PPCContext& ctx, uint8_t* base, uint32_t task) {
    auto& memory = *sfr::active_memory;
    ctx.r3.u64 = 0x82224350;  // the profiler's label for the stage
    sub_828057F0(ctx, base);
    ctx.r3.u64 = memory.load<uint32_t>(task + 148);
    sub_828039F8(ctx, base);
    for (uint32_t view = 0; view < 2; ++view) {
        // The camera of the view (+432: its position), as 82269550 finds it.
        const uint32_t cameras = memory.load<uint32_t>(memory.load<uint32_t>(0x83E516A0) + 116);
        const uint32_t camera = memory.load<uint32_t>(memory.load<uint32_t>(cameras + 8) + memory.load<uint32_t>(cameras + 28) * 4);
        ctx.r3.u64 = memory.load<uint32_t>(0x83E52FBC);
        ctx.r4.u64 = view;
        ctx.r5.u64 = camera + 432;
        sub_82272B48(ctx, base);
    }
    sub_822254E8(ctx, base);
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
// SFR_TEST_DRAW_EVERY=<n>: only every n-th race frame's drawing is watched
// (until the next frame's), so a whole race takes less time.
bool sampled(uint32_t race_frames) {
    static const uint32_t every = [] { const char* t = std::getenv("SFR_TEST_DRAW_EVERY"); return t ? (std::max)(1u, uint32_t(std::atoi(t))) : 1u; }();
    return race_frames % every == 0;
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
    // (Physical memory read-only stopped races: the host's file reads into
    // it fail. Objects there are compared by kind, below, instead.)
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

// SFR_TEST_DRAW_READS_KIND=<vtable>[+<vtable>...] (hex; with
// SFR_TEST_DRAW_READS=1): watches only what those kinds' drawing writes,
// told apart task by task (the written pages are made read-only again after
// each task's drawing; physical memory, where the task objects are, is
// compared before and after), every word it changed, also one it leaves the same
// each frame that something else changes in between (the watch above leaves
// those out). Each DRAW_READS entry is rip=<reader>:<kind>+<offset in the
// task, or the address>@<kind whose method was running>/<phase key>/t<guest
// thread> (0/0: outside the task manager).
const std::vector<uint32_t>& watched_kinds() {
    static const std::vector<uint32_t> kinds = [] {
        std::vector<uint32_t> list;
        if (const char* t = std::getenv("SFR_TEST_DRAW_READS_KIND"))
            for (char* p = const_cast<char*>(t); *p;) {
                list.push_back(uint32_t(std::strtoul(p, &p, 16)));
                if (*p) ++p;
            }
        return list;
    }();
    return kinds;
}
bool by_kind() { return !watched_kinds().empty(); }
bool watched_kind(uint32_t kind) {
    return std::find(watched_kinds().begin(), watched_kinds().end(), kind) != watched_kinds().end();
}
void before_task(uint8_t* base, uint32_t kind) {
    if (watched_kind(kind)) sfr::draw_reads::snapshot_physical(base, sfr::active_memory->committed_ranges());
}
void after_task(uint8_t* base, uint32_t task, uint32_t kind) {
    const bool keep = watched_kind(kind);
    if (keep)
        sfr::draw_reads::diff_physical(base, [&](uint32_t address) {
            words.push_back({address, kind, address - task < 0x2000u ? address - task : address, task});
        });
    // Another kind's drawing that changes a word afterwards leaves it as
    // that drawing wants it: it no longer depends on these kinds'.
    std::unordered_map<uint32_t, bool> replaced;
    sfr::draw_reads::rearm([&](const sfr::draw_reads::CapturedPage& page) {
        if (page.address == 0x82AF6000u) return;
        for (uint32_t offset = 0; offset < 4096; offset += 4)
            if (std::memcmp(base + page.address + offset, page.before + offset, 4)) {
                const uint32_t address = page.address + offset;
                if (keep) words.push_back({address, kind, address - task < 0x2000u ? address - task : address, task});
                else replaced[address] = true;
            }
    });
    if (!replaced.empty())
        std::erase_if(words, [&](const sfr::draw_reads::Word& w) { return replaced.count(w.address) != 0; });
}
void after_kind_drawing(uint8_t* base) {
    sfr::draw_reads::end_capture();
    // A word several tasks changed: the last one's.
    std::unordered_map<uint32_t, size_t> last;
    for (size_t i = 0; i < words.size(); ++i) last[words[i].address] = i;
    std::vector<sfr::draw_reads::Word> unique;
    for (size_t i = 0; i < words.size(); ++i)
        if (last[words[i].address] == i) unique.push_back(words[i]);
    words.clear();
    sfr::draw_reads::begin(base, std::move(unique), [] { return sfr::current_guest_thread_id(); });
}
}
}

// SFR_TEST_TASK_TIMES=1 (testing): where a race frame's task-manager time
// goes, by phase function, key and task kind, for drawn and undrawn frames
// apart; every 600 race frames, TASK_TIMES lines (ms per frame).
namespace task_times {
bool enabled() {
    static const bool on = [] { const char* t = std::getenv("SFR_TEST_TASK_TIMES"); return t && *t != '0'; }();
    return on;
}
struct Entry { uint64_t ns = 0, calls = 0, host_ns = 0, draws = 0; };
struct Table { std::unordered_map<uint64_t, Entry> entries; uint64_t frames = 0, manager_ns = 0, manager_calls = 0; };
Table tables[2];  // [drawn]
uint64_t now() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
uint64_t key_of(uint32_t function, uint32_t key, uint32_t kind) { return (uint64_t(function) << 32) ^ (uint64_t(key) << 28) ^ kind; }
// host_ns and draws: the draw calls' host work inside (guest_graphics_hooks.cpp).
void add(bool drawn, uint32_t function, uint32_t key, uint32_t kind, uint64_t ns, uint64_t host_ns = 0, uint64_t draws = 0) {
    auto& entry = tables[drawn].entries[key_of(function, key, kind)];
    entry.ns += ns; ++entry.calls; entry.host_ns += host_ns; entry.draws += draws;
}
// Keys keep function, key and kind recoverable: stored alongside.
std::unordered_map<uint64_t, std::array<uint32_t, 3>> names;
void name(uint32_t function, uint32_t key, uint32_t kind) { names.try_emplace(key_of(function, key, kind), std::array<uint32_t, 3>{function, key, kind}); }
void report(uint32_t race_frames) {
    for (int drawn = 0; drawn < 2; ++drawn) {
        auto& table = tables[drawn];
        if (!table.frames) continue;
        std::vector<std::pair<uint64_t, uint64_t>> order;
        uint64_t total = 0, host = 0, draws = 0;
        for (const auto& [k, e] : table.entries) { order.push_back({e.ns, k}); total += e.ns; host += e.host_ns; draws += e.draws; }
        std::sort(order.rbegin(), order.rend());
        std::string text;
        char item[128];
        const double n = double(table.frames);
        for (size_t i = 0; i < order.size() && i < 24; ++i) {
            const auto& e = table.entries[order[i].second];
            const auto& nm = names[order[i].second];
            std::snprintf(item, sizeof item, " %08x/%u/%08x=%.3f/%.1f/host%.3f/draws%.1f", nm[0], nm[1], nm[2], double(e.ns) / 1e6 / n,
                          double(e.calls) / n, double(e.host_ns) / 1e6 / n, double(e.draws) / n);
            text += item;
        }
        std::fprintf(stderr, "TASK_TIMES race_frame=%u drawn=%d frames=%llu manager_ms=%.3f managers=%.2f tasks_ms=%.3f host_ms=%.3f draws=%.1f%s\n",
                     race_frames, drawn, static_cast<unsigned long long>(table.frames), double(table.manager_ns) / 1e6 / n,
                     double(table.manager_calls) / n, double(total) / 1e6 / n, double(host) / 1e6 / n, double(draws) / n, text.c_str());
        table = Table{};
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
    const bool timing = race_frame && task_times::enabled();
    const bool drawn_frame = !race_frame || !sfr::race_frameskip_enabled() || sfr::race_frame_drawn();
    const uint64_t manager_start = timing ? task_times::now() : 0;
    struct ManagerClock {
        bool on; bool drawn; uint64_t start;
        ~ManagerClock() {
            if (!on) return;
            auto& table = task_times::tables[drawn];
            table.manager_ns += task_times::now() - start;
            ++table.manager_calls;
        }
    } manager_clock{timing, drawn_frame, manager_start};
    if (!skip_drawing && !watching && !timing) {
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
        if (watching && function == draw_method && key == 4 && !watch::sampled(race_frames)) sfr::draw_reads::end();
        if (watching && function == draw_method && key == 4 && watch::sampled(race_frames)) {
            std::vector<uint32_t> tasks;
            for (uint32_t element = memory.load<uint32_t>(end); element != end; element = memory.load<uint32_t>(element))
                tasks.push_back(memory.load<uint32_t>(element + 8));
            watch::before_drawing(base, watch::by_kind() ? std::vector<uint32_t>{} : tasks);
        }
        if (memory.load<uint32_t>(list + 4) == 0) {
        } else if (key == 1) {
            if (!memory.load<uint8_t>(manager + 192)) {
                ctx.r3.u64 = manager;
                ctx.r4.u64 = node;
                ctx.r5.u64 = list;
                if (watching) sfr::draw_reads::set_reader(0, key);
                const uint64_t start = timing ? task_times::now() : 0;
                sub_8249F048(ctx, base);
                if (timing) { task_times::name(function, key, 0); task_times::add(drawn_frame, function, key, 0, task_times::now() - start); }
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
                    const uint32_t task_kind = memory.load<uint32_t>(task);
                    const bool areas_only = skip_drawing && function == draw_method && stage_areas_only() &&
                                            !kept_when_undrawn(task_kind) && memory.load<uint32_t>(task_kind + 12) == stage_draw;
                    ctx.r3.u64 = task;
                    const bool watch_task = watching && watch::by_kind() && function == draw_method && watch::sampled(race_frames);
                    if (watching) sfr::draw_reads::set_reader(task_kind, key);
                    if (watch_task) watch::before_task(base, task_kind);
                    const uint64_t start = timing ? task_times::now() : 0;
                    const uint64_t host_start = timing ? sfr::draw_call_host_ns.load(std::memory_order_relaxed) : 0;
                    const uint64_t draws_start = timing ? sfr::draw_call_count.load(std::memory_order_relaxed) : 0;
                    if (areas_only) find_stage_areas(ctx, base, task);
                    else sfr::call_indirect(ctx, base, function);
                    if (watch_task) watch::after_task(base, task, task_kind);
                    if (timing) {
                        const uint32_t kind = memory.load<uint32_t>(task);
                        task_times::name(function, key, kind);
                        task_times::add(drawn_frame, function, key, kind, task_times::now() - start,
                                        sfr::draw_call_host_ns.load(std::memory_order_relaxed) - host_start,
                                        sfr::draw_call_count.load(std::memory_order_relaxed) - draws_start);
                    }
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
            if (!watch::sampled(race_frames)) {
            } else if (watch::by_kind()) watch::after_kind_drawing(base);
            else watch::after_drawing(base);
            if (race_frames % 600 == 0) sfr::draw_reads::report(race_frames);
        }
        node = advance();
    }
    memory.store<uint32_t>(manager + 188, 0);
    if (watching) sfr::draw_reads::set_reader(0, 0);
    ctx.r1.u64 = saved_r1;
    if (timing) {
        static uint32_t last_present = 0;
        const uint32_t present = sfr::present_count.load(std::memory_order_relaxed);
        if (present != last_present) {
            last_present = present;
            ++task_times::tables[drawn_frame].frames;
        }
        if (race_frames % 600 == 0) task_times::report(race_frames);
    }
}
