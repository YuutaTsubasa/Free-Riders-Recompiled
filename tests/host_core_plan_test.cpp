#include "host_core_plan.h"
#include <iostream>
#include <stdexcept>
#if defined(__linux__)
#include <condition_variable>
#include <thread>
#endif

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

#if defined(__linux__)
// A kept worker the system lets onto every core is placed again, once, and
// one that has left is no longer touched (Issue #176). The role's mask comes
// from SFR_TEST_WORKER_CPUS (the first allowed core), as on a phone.
void placement_is_restored() {
    const uint64_t all = sfr::core_plan_detail::allowed_of(0);
    if (!all || !(all & (all - 1))) {
        std::cout << "one core only: placement restore not tested\n";
        return;
    }
    const uint64_t first = all & (~all + 1);
    char text[24];
    std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(first));
    setenv("SFR_TEST_WORKER_CPUS", text, 1);
    std::mutex lock;
    std::condition_variable changed;
    pid_t tid = 0;
    bool leave = false;
    std::thread worker([&] {
        require(sfr::place_host_thread(sfr::HostThreadRole::worker) == first, "the worker is placed by its test mask");
        {
            std::lock_guard guard(lock);
            tid = sfr::core_plan_detail::current_tid();
        }
        changed.notify_all();
        std::unique_lock wait(lock);
        changed.wait(wait, [&] { return leave; });
        sfr::forget_placement();
    });
    {
        std::unique_lock wait(lock);
        changed.wait(wait, [&] { return tid != 0; });
    }
    require(sfr::restore_placement().empty(), "placed as planned: nothing to say");
    require(sfr::core_plan_detail::narrow_to(all, tid) == all, "the system resets the worker to every core");
    const std::string line = sfr::restore_placement();
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(first));
    require(line.rfind("CORE_PLAN_REAPPLIED workers=1:", 0) == 0 && line.find(">" + std::string(text)) != std::string::npos,
            "a reset worker is placed again and logged");
    require(sfr::core_plan_detail::allowed_of(tid) == first, "the worker is back on its core");
    require(sfr::restore_placement().empty(), "once placed again, nothing more to say");
    {
        std::lock_guard guard(lock);
        leave = true;
    }
    changed.notify_all();
    worker.join();
    require(sfr::core_plan_detail::placed.empty(), "a thread that left is forgotten");
    unsetenv("SFR_TEST_WORKER_CPUS");
}
#endif
int main() {
    try {
        // AYN Thor (Snapdragon 8 Gen 2): cpu_capacity 280 x3, 855 x4, 1024.
        long thor[64] = {280, 280, 280, 855, 855, 855, 855, 1024};
        const auto split = sfr::split_cores(thor);
        require(split.fastest == 0x80 && split.rest == 0x7f, "Thor: CPU 7 alone is the fastest; the rest keep the render thread");
        // Helio G99: two A76 beside six A55 (capacities as reported).
        long g99[64] = {350, 350, 350, 350, 350, 350, 1024, 1024};
        const auto little = sfr::split_cores(g99);
        require(!little.fastest && !little.rest, "only little cores besides the fastest: placement stays the system's");
        // Alike cores, and no report at all.
        long same[64] = {1024, 1024, 1024, 1024};
        const auto alike = sfr::split_cores(same);
        require(!alike.fastest && !alike.rest, "alike cores: nothing changes");
        long none[64] = {};
        const auto unknown = sfr::split_cores(none);
        require(!unknown.fastest && !unknown.rest, "no report: nothing changes");
        // Snapdragon 8 Gen 1 by clock (no capacity): 3.0 GHz, 2.5 GHz x3, 1.8 GHz x4.
        long clocks[64] = {1785600, 1785600, 1785600, 1785600, 2496000, 2496000, 2496000, 2995200};
        const auto gen1 = sfr::split_cores(clocks);
        require(gen1.fastest == 0x80 && gen1.rest == 0x7f, "a second tier at 83% of the clock counts");
        // Two equally fastest cores.
        long two[64] = {400, 400, 900, 900, 1024, 1024};
        const auto pair = sfr::split_cores(two);
        require(pair.fastest == 0x30 && pair.rest == 0x0f, "every core at the top speed is among the fastest");
        // Big cores: half the fastest's speed or more, four of them at least, beside little ones.
        require(sfr::big_cores(thor) == 0xf8, "Thor: CPUs 3-7 are big, 0-2 little");
        long tensor[64] = {256, 256, 256, 256, 740, 740, 740, 1024};  // a Tensor G4-like 4+3+1
        require(sfr::big_cores(tensor) == 0xf0, "Tensor-like: CPUs 4-7 are big");
        require(!sfr::big_cores(g99), "two big cores are too few to hold the busy threads");
        require(!sfr::big_cores(same) && !sfr::big_cores(none), "no little cores: nothing to avoid");
        // Roles: the plan's choice, a test mask replacing it, the process test mask limiting both.
        using sfr::HostThreadRole;
        require(sfr::host_role_mask(HostThreadRole::render, 1, thor, 0, 0) == 0x7f, "plan 1: render off CPU 7");
        require(sfr::host_role_mask(HostThreadRole::main, 1, thor, 0, 0) == 0, "plan 1 leaves the main thread");
        require(sfr::host_role_mask(HostThreadRole::worker, 1, thor, 0, 0) == 0, "plan 1 leaves the workers");
        require(sfr::host_role_mask(HostThreadRole::main, 2, thor, 0, 0) == 0x80, "plan 2: main on CPU 7");
        require(sfr::host_role_mask(HostThreadRole::main, 8, tensor, 0, 0) == 0xf0, "plan 8: main off little cores");
        require(sfr::host_role_mask(HostThreadRole::worker, 4, tensor, 0, 0) == 0xf0, "plan 4: workers off little cores");
        require(sfr::host_role_mask(HostThreadRole::worker, 4, g99, 0, 0) == 0, "plan 4 needs four big cores");
        require(sfr::host_role_mask(HostThreadRole::render, 17, tensor, 0, 0) == 0x70, "plan 1+16: render on the middle tier");
        require(sfr::host_role_mask(HostThreadRole::render, 16, thor, 0, 0) == 0xf8, "plan 16 alone: render on big cores");
        require(sfr::host_role_mask(HostThreadRole::worker, 1, thor, 0x07, 0) == 0x07, "a test mask replaces the plan");
        require(sfr::host_role_mask(HostThreadRole::render, 1, thor, 0, 0xf7) == 0x77, "the process test mask limits the plan");
        require(sfr::host_role_mask(HostThreadRole::main, 2, thor, 0, 0x07) == 0x07,
                "a plan mask outside the process test mask falls back to it");
        // Plan 32: a lone fastest core well ahead of the rest takes the main thread.
        long pixel[64] = {191, 191, 191, 191, 741, 741, 741, 1024};  // Pixel 10a, as its game.log reports
        require(sfr::lone_fastest_core(pixel) == 0x80, "Pixel 10a: the X4 is alone and well ahead");
        require(!sfr::lone_fastest_core(thor), "Thor: the next tier has 83%, so no lone core");
        require(!sfr::lone_fastest_core(clocks), "8 Gen 1 by clock: 83%, no lone core");
        require(!sfr::lone_fastest_core(two) && !sfr::lone_fastest_core(same) && !sfr::lone_fastest_core(none),
                "two fastest, alike or unreported cores: no lone core");
        require(!sfr::lone_fastest_core(g99), "only little cores besides the fastest: placement stays the system's");
        require(sfr::host_role_mask(HostThreadRole::main, 45, pixel, 0, 0) == 0x80, "plan 45 on the Pixel: main on the X4");
        require(sfr::host_role_mask(HostThreadRole::worker, 45, pixel, 0, 0) == 0x70, "plan 45 on the Pixel: workers on the A720s");
        require(sfr::host_role_mask(HostThreadRole::render, 45, pixel, 0, 0) == 0x7f, "plan 45 on the Pixel: render off the X4");
        require(sfr::host_role_mask(HostThreadRole::main, 45, thor, 0, 0) == 0xf8, "plan 45 on the Thor: as plan 13");
        require(sfr::host_role_mask(HostThreadRole::worker, 45, thor, 0, 0) == 0xf8, "plan 45 on the Thor: workers as plan 13");
        require(sfr::host_role_mask(HostThreadRole::main, 13, pixel, 0, 0) == 0xf0, "without bit 32 the Pixel is as before");
        // Placing again: only a thread the system let onto cores its role avoids (Issue #176).
        require(sfr::needs_placement(0xff, 0x80), "Pixel main back from another cpuset on every core: place again");
        require(sfr::needs_placement(0x7f, 0x80), "Pixel main in a cpuset without the X4: try again (refused there)");
        require(sfr::needs_placement(0xff, 0x70) && sfr::needs_placement(0x07, 0xf8), "workers reset: place again");
        require(!sfr::needs_placement(0x80, 0x80) && !sfr::needs_placement(0xf8, 0xf8), "placed as planned: leave it");
        require(!sfr::needs_placement(0x58, 0xf8) && !sfr::needs_placement(0x38, 0xf8),
                "Thor main with cores paused by the system: narrower than the plan, leave it");
        require(!sfr::needs_placement(0, 0x80), "affinity unknown: leave it");
        require(!sfr::needs_placement(0xff, 0), "no plan for the role: leave it");
        require(sfr::parse_cpu_mask("0xf7") == 0xf7 && sfr::parse_cpu_mask("78") == 0x78 && !sfr::parse_cpu_mask(""),
                "masks are hexadecimal");
#if defined(__linux__)
        placement_is_restored();
#endif
        std::cout << "host core plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
