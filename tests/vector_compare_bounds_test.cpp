#include "vector_compare_bounds.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::uint32_t one(float value, float bound) {
    const float values[4] = {value, 0, 0, 0};
    const float bounds[4] = {bound, 1, 1, 1};
    std::uint32_t result[4] = {};
    sfr::vector_compare_bounds(values, bounds, result);
    return result[0];
}

constexpr std::uint32_t above = 0x80000000u, below = 0x40000000u;

void inside_the_bound_is_zero() {
    require(one(0.5f, 1.0f) == 0, "below the bound and above its negative is inside");
    require(one(-0.5f, 1.0f) == 0, "the negative side counts too");
    require(one(1.0f, 1.0f) == 0, "the bound itself is inside");
    require(one(-1.0f, 1.0f) == 0, "and so is its negative");
}

void outside_says_which_way() {
    require(one(1.5f, 1.0f) == above, "past the bound sets the top bit");
    require(one(-1.5f, 1.0f) == below, "past its negative sets the next one");
}

// A negative bound has nothing inside it: a value cannot be both at most b
// and at least -b when b is below zero.
void a_negative_bound_holds_nothing() {
    require(one(0.0f, -1.0f) == (above | below), "zero is outside a negative bound both ways");
    require(one(-5.0f, -1.0f) == below, "a value under a negative bound is only below it: -5 is at most -1");
}

void a_nan_is_outside_both_ways() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    require(one(nan, 1.0f) == (above | below), "a NaN value compares false either way");
    require(one(1.0f, nan) == (above | below), "and so does a NaN bound");
}

void infinities_compare_as_numbers() {
    const float infinity = std::numeric_limits<float>::infinity();
    require(one(infinity, 1.0f) == above, "infinity is past any finite bound");
    require(one(1.0f, infinity) == 0, "and an infinite bound holds everything finite");
}

void every_element_is_its_own() {
    const float values[4] = {2.0f, -2.0f, 0.25f, 0.0f};
    const float bounds[4] = {1.0f, 1.0f, 1.0f, -1.0f};
    std::uint32_t result[4] = {};
    sfr::vector_compare_bounds(values, bounds, result);
    require(result[0] == above, "the first is above");
    require(result[1] == below, "the second below");
    require(result[2] == 0, "the third inside");
    require(result[3] == (above | below), "the fourth has a negative bound");
}
}

int main() {
    try {
        inside_the_bound_is_zero();
        outside_says_which_way();
        a_negative_bound_holds_nothing();
        a_nan_is_outside_both_ways();
        infinities_compare_as_numbers();
        every_element_is_its_own();
        std::cout << "Vector compare bounds checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
