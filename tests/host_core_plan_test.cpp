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
        std::cout << "host core plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
