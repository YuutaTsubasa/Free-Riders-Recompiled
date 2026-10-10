#include "vector_unpack.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void halves_convert_exactly() {
    require(sfr::half_to_float(0x3C00) == 1.0f, "one");
    require(sfr::half_to_float(0xC000) == -2.0f, "minus two");
    require(sfr::half_to_float(0x3555) == 0.333251953125f, "a third, rounded as a half");
    require(sfr::half_to_float(0x7BFF) == 65504.0f, "the largest half");
    require(sfr::half_to_float(0x0001) == std::ldexp(1.0f, -24), "the smallest subnormal");
    require(sfr::half_to_float(0x03FF) == std::ldexp(1023.0f, -24), "the largest subnormal");
    require(std::signbit(sfr::half_to_float(0x8000)) && sfr::half_to_float(0x8000) == 0.0f, "negative zero");
    require(std::isinf(sfr::half_to_float(0x7C00)), "infinity");
    require(std::isnan(sfr::half_to_float(0x7E00)), "not a number");
}

// Host element i is guest element 7 - i (halves) or 3 - i (words).
void four_halves_come_from_the_last_two_words() {
    // Guest halves 4..7 are X, Y, Z, W = 1, 2, 3, 4; 0..3 are noise.
    const uint16_t source[8] = {0x4400, 0x4200, 0x4000, 0x3C00, 0x7777, 0x7777, 0x7777, 0x7777};
    float destination[4];
    sfr::vector_unpack_half(source, destination, 4);
    require(destination[3] == 1.0f && destination[2] == 2.0f && destination[1] == 3.0f &&
                destination[0] == 4.0f, "X, Y, Z, W in guest order");
}

void two_halves_come_from_the_last_word() {
    // Guest halves 6 and 7 are X and Y = 5 and -1.
    const uint16_t source[8] = {0xBC00, 0x4500, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777};
    float destination[4];
    sfr::vector_unpack_half(source, destination, 2);
    require(destination[3] == 5.0f && destination[2] == -1.0f && destination[1] == 0.0f &&
                destination[0] == 1.0f, "X, Y, 0, 1 in guest order");
}

void the_source_may_be_the_destination() {
    union { uint16_t halves[8]; float floats[4]; } v{};
    v.halves[0] = 0x4000;  // guest half 7: Y = 2
    v.halves[1] = 0x3C00;  // guest half 6: X = 1
    sfr::vector_unpack_half(v.halves, v.floats, 2);
    require(v.floats[3] == 1.0f && v.floats[2] == 2.0f, "read before written");
}
}

int main() {
    try {
        halves_convert_exactly();
        four_halves_come_from_the_last_two_words();
        two_halves_come_from_the_last_word();
        the_source_may_be_the_destination();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "vector unpack tests passed\n";
}
