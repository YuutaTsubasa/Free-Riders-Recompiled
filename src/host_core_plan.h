#pragma once
// Which of a phone's cores the busiest host threads run on. Phones have one
// or a few cores faster than the rest: an AYN Thor's CPU 7 (capacity 1024,
// 3.19 GHz) beside CPUs 3-6 (855, 2.80 GHz) and 0-2 (280). The title's main
// thread decides the frame rate, yet Android placed the render thread, which
// mostly waits for work, on CPU 7 four fifths of a race; kept off it, the
// main thread took it and a Dolphin Resort race went from 52 to 60 fps.
// A Pixel 10a (Tensor G4: one X4, three A720, four A520) raced at 0.85 of
// real time with ~17 ms of CPU work in an undrawn frame (Issue #176), a Thor
// at 1.00 with ~10. On the Thor, the same race with every guest thread but
// the main one held to the little cores gave undrawn frames of 16.8 ms and
// the main thread waiting ~7.7 ms a frame for them: the title's main thread
// waits on its job helpers, so a helper on a little core stalls the frame.
// Snapdragon's scheduler (WALT) keeps them off the little cores by itself;
// the Pixel's apparently did not (to be confirmed on a Tensor). Bits 4 and 8
// keep them off the little cores on any phone with four or more big ones.
// SFR_CORE_PLAN (bits; default 1|4|8 = 13 on Android, 0 elsewhere; 0 = the
// system places every thread):
//   1  the render thread avoids the fastest cores,
//   2  the title's main thread runs only on the fastest cores,
//   4  the other guest threads avoid the little cores,
//   8  the title's main thread avoids the little cores,
//  16  the render thread avoids the little cores too.
// Bit 1 only where cores other than the fastest have at least 70% of their
// capacity: a phone whose second tier is little cores (a Helio G99's A55s
// beside two A76s) keeps the render thread's placement to the system. Bits
// 4, 8 and 16 only where at least four cores have half the fastest's capacity
// or more (the "big" cores) and some have less (the "little" ones).
//
// Test switches that emulate another phone's cores on this one (hex CPU
// masks; unset = off):
//   SFR_TEST_CPUS         every thread of the process, from the start (so
//                         every thread created later too); the masks below
//                         and the plan's are limited to it,
//   SFR_TEST_MAIN_CPUS    the title's main thread,
//   SFR_TEST_WORKER_CPUS  every other guest thread,
//   SFR_TEST_RENDER_CPUS  the render thread.
// A thread's test mask replaces the plan's choice for it.
// Linux and Android only; elsewhere the calls do nothing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#if defined(__linux__)
#include <dirent.h>
#include <fstream>
#include <sched.h>
#endif

namespace sfr {

inline unsigned host_core_plan() {
    static const unsigned plan = [] {
        const char* text = std::getenv("SFR_CORE_PLAN");
        if (text && *text) return unsigned(std::strtoul(text, nullptr, 10));
#if defined(__ANDROID__)
        return 1u | 4u | 8u;
#else
        return 0u;
#endif
    }();
    return plan;
}

// Cores split into the fastest (highest speed: cpu_capacity, or cpufreq
// maximum where there is no capacity) and the rest, among which the system
// still chooses. Both 0 where the cores are alike, where nothing is reported
// (speed 0), or where no other core has 70% of the fastest's speed.
struct CoreSplit { uint64_t fastest = 0, rest = 0; };
inline CoreSplit split_cores(const long (&speed)[64]) {
    long best = 0;
    for (const long value : speed)
        if (value > best) best = value;
    CoreSplit split;
    if (best <= 0) return split;
    bool second_tier = false;
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (speed[cpu] <= 0) continue;
        if (speed[cpu] == best) split.fastest |= uint64_t{1} << cpu;
        else {
            split.rest |= uint64_t{1} << cpu;
            second_tier = second_tier || speed[cpu] * 10 >= best * 7;
        }
    }
    if (!second_tier) split = {};
    return split;
}

// The cores with at least half the fastest's speed, where at least four
// have it and some other core has less; 0 otherwise (no little cores to
// avoid, or too few big ones to hold the busy threads).
inline uint64_t big_cores(const long (&speed)[64]) {
    long best = 0;
    for (const long value : speed)
        if (value > best) best = value;
    if (best <= 0) return 0;
    uint64_t big = 0;
    int count = 0;
    bool little = false;
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (speed[cpu] <= 0) continue;
        if (speed[cpu] * 2 >= best) big |= uint64_t{1} << cpu, ++count;
        else little = true;
    }
    return little && count >= 4 ? big : 0;
}

enum class HostThreadRole { main, worker, render };

// What a thread of the role is allowed: the test mask, or the plan's choice,
// limited to the process test mask. 0 = leave the thread as it is.
inline uint64_t host_role_mask(HostThreadRole role, unsigned plan, const long (&speed)[64],
                               uint64_t forced, uint64_t limit) {
    uint64_t mask = forced;
    if (!mask) {
        const CoreSplit split = split_cores(speed);
        const uint64_t big = big_cores(speed);
        switch (role) {
        case HostThreadRole::main:
            if (plan & 2) mask = split.fastest;
            else if (plan & 8) mask = big;
            break;
        case HostThreadRole::worker:
            if (plan & 4) mask = big;
            break;
        case HostThreadRole::render:
            if (plan & 1) mask = split.rest;
            if ((plan & 16) && big) mask = mask ? (mask & big ? mask & big : mask) : big;
            break;
        }
    }
    if (limit && mask) mask = mask & limit ? mask & limit : limit;
    return mask;
}

inline uint64_t parse_cpu_mask(const char* text) {
    return text && *text ? uint64_t(std::strtoull(text, nullptr, 16)) : 0;
}

#if defined(__linux__)
namespace core_plan_detail {
inline long read_number(const std::string& path) {
    long value = 0;
    std::ifstream file(path);
    if (file) file >> value;
    return value;
}
struct Speeds { long speed[64] = {}; };
inline const Speeds& system_speeds() {
    static const Speeds speeds = [] {
        Speeds result;
        for (int cpu = 0; cpu < 64; ++cpu) {
            const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
            result.speed[cpu] = read_number(base + "/cpu_capacity");
            if (result.speed[cpu] <= 0) result.speed[cpu] = read_number(base + "/cpufreq/cpuinfo_max_freq");
        }
        return result;
    }();
    return speeds;
}
inline CoreSplit system_split() { return split_cores(system_speeds().speed); }
inline cpu_set_t set_of(uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64; ++cpu)
        if (mask >> cpu & 1) CPU_SET(cpu, &set);
    return set;
}
// tid 0 = the calling thread.
inline uint64_t narrow_to(uint64_t mask, pid_t tid = 0) {
    if (!mask) return 0;
    const cpu_set_t set = set_of(mask);
    return sched_setaffinity(tid, sizeof(set), &set) == 0 ? mask : 0;
}
inline const char* role_test_switch(HostThreadRole role) {
    switch (role) {
    case HostThreadRole::main: return "SFR_TEST_MAIN_CPUS";
    case HostThreadRole::worker: return "SFR_TEST_WORKER_CPUS";
    case HostThreadRole::render: return "SFR_TEST_RENDER_CPUS";
    }
    return "";
}
}

inline uint64_t test_process_cpus() {
    static const uint64_t mask = parse_cpu_mask(std::getenv("SFR_TEST_CPUS"));
    return mask;
}

// SFR_TEST_CPUS: limits every thread the process has now; threads created
// afterwards inherit it from their creators. Returns the mask, or 0.
inline uint64_t apply_test_process_cpus() {
    const uint64_t mask = test_process_cpus();
    if (!mask) return 0;
    if (DIR* tasks = opendir("/proc/self/task")) {
        while (const dirent* entry = readdir(tasks))
            if (entry->d_name[0] != '.') core_plan_detail::narrow_to(mask, pid_t(std::atoi(entry->d_name)));
        closedir(tasks);
    }
    return mask;
}

// The mask the plan and the test switches give a thread of the role; 0 =
// leave it to the system (or to what it inherited).
inline uint64_t host_role_mask(HostThreadRole role) {
    return host_role_mask(role, host_core_plan(), core_plan_detail::system_speeds().speed,
                          parse_cpu_mask(std::getenv(core_plan_detail::role_test_switch(role))),
                          test_process_cpus());
}

// Places the calling thread (tid 0) or thread tid by its role; returns the
// new mask, or 0 where nothing changed.
inline uint64_t place_host_thread(HostThreadRole role, pid_t tid = 0) {
    return core_plan_detail::narrow_to(host_role_mask(role), tid);
}

// One line for the log: the plan, each core's speed and the masks it gives
// (a player's game.log then shows how the plan saw their phone).
inline std::string describe_core_plan() {
    const auto& speed = core_plan_detail::system_speeds().speed;
    std::string text = "CORE_PLAN plan=" + std::to_string(host_core_plan()) + " speeds=";
    for (int cpu = 0; cpu < 64 && speed[cpu] > 0; ++cpu) text += (cpu ? "," : "") + std::to_string(speed[cpu]);
    const auto hex = [](uint64_t mask) {
        char buffer[24];
        std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(mask));
        return std::string(buffer);
    };
    text += " fastest=" + hex(split_cores(speed).fastest) + " big=" + hex(big_cores(speed)) +
            " main=" + hex(host_role_mask(HostThreadRole::main)) + " workers=" + hex(host_role_mask(HostThreadRole::worker)) +
            " render=" + hex(host_role_mask(HostThreadRole::render));
    return text;
}
#else
inline uint64_t test_process_cpus() { return 0; }
inline uint64_t apply_test_process_cpus() { return 0; }
inline uint64_t host_role_mask(HostThreadRole) { return 0; }
inline uint64_t place_host_thread(HostThreadRole) { return 0; }
inline std::string describe_core_plan() { return {}; }
#endif

}
