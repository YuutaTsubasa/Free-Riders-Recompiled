#pragma once
// Which of a phone's cores the busiest host threads run on. Phones have one
// or a few cores faster than the rest: an AYN Thor's CPU 7 (capacity 1024,
// 3.19 GHz) beside CPUs 3-6 (855, 2.80 GHz) and 0-2 (280). The title's main
// thread decides the frame rate, yet Android placed the render thread, which
// mostly waits for work, on CPU 7 four fifths of a race; kept off it, the
// main thread took it and a Dolphin Resort race went from 52 to 60 fps.
// SFR_CORE_PLAN (bits; default 1 on Android, 0 elsewhere; 0 = the system
// places every thread):
//   1  the render thread avoids the fastest cores,
//   2  the title's main thread runs only on the fastest cores.
// Only where cores other than the fastest have at least 70% of their
// capacity: a phone whose second tier is little cores (a Helio G99's A55s
// beside two A76s) keeps the render thread's placement to the system.
// Linux and Android only; elsewhere the calls do nothing.
#include <cstdint>
#include <cstdlib>
#if defined(__linux__)
#include <fstream>
#include <sched.h>
#include <string>
#endif

namespace sfr {

inline unsigned host_core_plan() {
    static const unsigned plan = [] {
        const char* text = std::getenv("SFR_CORE_PLAN");
        if (text && *text) return unsigned(std::strtoul(text, nullptr, 10));
#if defined(__ANDROID__)
        return 1u;
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

#if defined(__linux__)
namespace core_plan_detail {
inline long read_number(const std::string& path) {
    long value = 0;
    std::ifstream file(path);
    if (file) file >> value;
    return value;
}
inline CoreSplit system_split() {
    long speed[64] = {};
    for (int cpu = 0; cpu < 64; ++cpu) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
        speed[cpu] = read_number(base + "/cpu_capacity");
        if (speed[cpu] <= 0) speed[cpu] = read_number(base + "/cpufreq/cpuinfo_max_freq");
    }
    return split_cores(speed);
}
inline uint64_t narrow_to(uint64_t mask) {
    if (!mask) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64; ++cpu)
        if (mask >> cpu & 1) CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0 ? mask : 0;
}
}

// Both return the calling thread's new mask, or 0 where nothing changed.
inline uint64_t avoid_fastest_host_processors() {
    return core_plan_detail::narrow_to(core_plan_detail::system_split().rest);
}
inline uint64_t prefer_fastest_host_processors() {
    return core_plan_detail::narrow_to(core_plan_detail::system_split().fastest);
}
#else
inline uint64_t avoid_fastest_host_processors() { return 0; }
inline uint64_t prefer_fastest_host_processors() { return 0; }
#endif

}
