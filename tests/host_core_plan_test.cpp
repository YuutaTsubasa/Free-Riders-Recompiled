#include "host_core_plan.h"
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
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
        require(sfr::parse_cpu_mask("0xf7") == 0xf7 && sfr::parse_cpu_mask("78") == 0x78 && !sfr::parse_cpu_mask(""),
                "masks are hexadecimal");
        std::cout << "host core plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
