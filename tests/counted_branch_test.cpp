#include "counted_branch.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct DispatchResult {
    unsigned selected;
    uint64_t counter;
};

DispatchResult run_original_chain(uint64_t initial) {
    uint64_t counter = initial;
    const bool condition = initial == 0;
    for (unsigned dispatch = 1; dispatch <= 7; ++dispatch) {
        if (sfr::branch_counter_zero_false(counter, condition)) return {dispatch, counter};
    }
    return {0, counter};
}

}

int main() {
    try {
        constexpr std::array<uint64_t, 4> counters{
            0, 1, 2, std::numeric_limits<uint64_t>::max()
        };
        for (bool condition : {false, true}) {
            for (uint64_t initial : counters) {
                uint64_t counter = initial;
                const bool branch = sfr::branch_counter_zero_false(counter, condition);
                require(counter == initial - 1, "counted branch always decrements the full 64-bit counter");
                require(branch == (initial == 1 && !condition), "counted branch uses low-32 zero and false condition");
            }
        }

        uint64_t counter = 0x100000001ull;
        require(sfr::branch_counter_zero_false(counter, false) && counter == 0x100000000ull,
                "low-32 zero branches while retaining high counter bits");
        counter = 0x100000000ull;
        require(!sfr::branch_counter_zero_false(counter, false) && counter == 0xffffffffull,
                "decrement is 64-bit rather than truncated to 32 bits");
        counter = 1;
        require(!sfr::branch_counter_zero_false(counter, true) && counter == 0,
                "true condition suppresses the branch but still decrements");

        for (uint64_t initial = 0; initial <= 8; ++initial) {
            const DispatchResult result = run_original_chain(initial);
            const unsigned expected_dispatch = initial >= 1 && initial <= 7 ? static_cast<unsigned>(initial) : 0;
            const uint64_t expected_counter = initial - (expected_dispatch == 0 ? 7 : expected_dispatch);
            require(result.selected == expected_dispatch, "seven-way chain selects the original dispatch target");
            require(result.counter == expected_counter, "seven-way chain preserves the final 64-bit count");
        }

        std::cout << "Counted branch checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
