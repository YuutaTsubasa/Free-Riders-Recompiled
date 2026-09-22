#include "integer_arithmetic.h"
#include "guest_memory.h"
#include <array>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        struct AddCase { uint64_t left, right, result; uint8_t carry; };
        constexpr AddCase add_cases[] = {
            {0xffffffffull, 1, 0x100000000ull, 1},
            {0xffffffff00000000ull, 0x100000000ull, 0, 0},
            {0x1234567800000001ull, 0x1111111100000002ull, 0x2345678900000003ull, 0},
            {UINT64_MAX, 1, 0, 1},
        };
        for (const auto& test : add_cases) {
            uint64_t left = test.left;
            uint64_t right = test.right;
            uint64_t destination = 0x5555555555555555ull;
            uint8_t carry = 0xa5;
            sfr::addc(destination, left, right, carry);
            require(destination == test.result, "addc computes a full 64-bit modulo result");
            require(carry == test.carry, "addc derives carry from the low 32-bit sum");
            require(left == test.left && right == test.right, "addc preserves both source values");

            const uint64_t mask = ~destination + destination + uint64_t(carry);
            require(mask == (test.carry ? 0 : UINT64_MAX),
                    "addc carry feeds the expected original self-subfe mask");
        }
        for (uint8_t old_carry : {uint8_t(0), uint8_t(1), uint8_t(255)}) {
            uint64_t destination = 9;
            uint8_t carry = old_carry;
            sfr::addc(destination, 2, 3, carry);
            require(destination == 5 && carry == 0, "addc ignores and overwrites the incoming carry value");
        }
        {
            uint64_t destination = 0xffffffffull;
            uint64_t right = 1;
            uint8_t carry = 0;
            sfr::addc(destination, destination, right, carry);
            require(destination == 0x100000000ull && right == 1 && carry == 1,
                    "addc supports destination aliasing the left source");
        }
        {
            uint64_t destination = 1;
            uint64_t left = UINT64_MAX;
            uint8_t carry = 255;
            sfr::addc(destination, left, destination, carry);
            require(destination == 0 && left == UINT64_MAX && carry == 1,
                    "addc supports destination aliasing the right source");
        }
        {
            uint64_t destination = UINT64_MAX;
            uint8_t carry = 0;
            sfr::addc(destination, destination, destination, carry);
            require(destination == UINT64_MAX - 1 && carry == 1,
                    "addc supports destination aliasing both source operands");
        }

        struct AddmeCase { uint64_t source, result; uint8_t carry, next; };
        constexpr AddmeCase addme_cases[] = {
            {0, UINT64_MAX, 0, 0}, {0, 0, 1, 1},
            {1, 0, 0, 1},
            {0x100000000ull, 0xffffffffull, 0, 0},
            {0x100000000ull, 0x100000000ull, 1, 1},
            {UINT64_MAX, UINT64_MAX - 1, 0, 1},
        };
        for (const auto& test : addme_cases) {
            uint64_t source = test.source;
            uint64_t destination = 0x5555555555555555ull;
            uint8_t carry = test.carry;
            sfr::addme(destination, source, carry);
            require(destination == test.result, "addme computes the full 64-bit modulo result");
            require(carry == test.next, "addme derives carry from the old carry and low 32-bit source");
            require(source == test.source, "addme preserves a distinct source register");

            uint64_t alias = test.source;
            carry = test.carry;
            sfr::addme(alias, alias, carry);
            require(alias == test.result && carry == test.next,
                    "addme supports destination aliasing its source register");
        }
        for (uint8_t invalid : {uint8_t(2), uint8_t(255)}) {
            uint64_t destination = 0x123456789abcdef0ull;
            const uint64_t source = 0xfedcba9876543210ull;
            uint8_t carry = invalid;
            bool stopped = false;
            try { sfr::addme(destination, source, carry); }
            catch (const sfr::RuntimeStop& error) { stopped = error.category == "arithmetic-carry"; }
            require(stopped && destination == 0x123456789abcdef0ull &&
                    source == 0xfedcba9876543210ull && carry == invalid,
                    "invalid addme carry stops before changing operands or state");
        }
        for (uint64_t original : {0ull, 1ull, 0x7fffffffull, 0x80000000ull, 0xffffffffull,
                                  0x1234567800000001ull, 0x1234567880000000ull}) {
            uint64_t mask = uint32_t(original) >> 31;
            uint8_t carry = uint32_t(original) == 0;
            sfr::addme(mask, mask, carry);
            const bool positive = static_cast<int32_t>(uint32_t(original)) > 0;
            require(mask == (positive ? UINT64_MAX : 0) && (original & mask) == (positive ? original : 0),
                    "original subfic sign-bit addme chain masks nonpositive values");
        }

        struct Case { uint64_t source; uint8_t carry; uint64_t result; uint8_t next; };
        constexpr Case cases[] = {
            {0, 0, UINT64_MAX, 0}, {0, 1, 0, 1},
            {1, 0, UINT64_MAX - 1, 0}, {1, 1, UINT64_MAX, 0},
            {UINT64_MAX, 0, 0, 0}, {UINT64_MAX, 1, 1, 0},
            {0x100000000ull, 0, 0xfffffffeffffffffull, 0},
            {0x100000000ull, 1, 0xffffffff00000000ull, 1},
            {0xffffffff00000000ull, 1, 0x100000000ull, 1},
            {0x8000000000000000ull, 1, 0x8000000000000000ull, 1},
            {0x8000000000000000ull, 0, 0x7fffffffffffffffull, 0},
            {0x7fffffffull, 1, 0xffffffff80000001ull, 0},
            {0x80000000ull, 1, 0xffffffff80000000ull, 0},
            {0x1234567800000000ull, 1, 0xedcba98800000000ull, 1},
        };
        for (const auto& test : cases) {
            std::array<uint64_t, 3> registers{0x123456789abcdef0ull, 0x5555, 0xfedcba9876543210ull};
            std::array<uint8_t, 3> flags{0xa5, test.carry, 0x5a};
            sfr::subfze(registers[1], test.source, flags[1]);
            require(registers[1] == test.result, "subfze computes the full unsigned 64-bit result");
            require(flags[1] == test.next, "subfze updates the low-32 carry");
            require(registers[0] == 0x123456789abcdef0ull && registers[2] == 0xfedcba9876543210ull,
                    "neighbor registers remain unchanged");
            require(flags[0] == 0xa5 && flags[2] == 0x5a, "neighbor flags remain unchanged");
            uint64_t alias = test.source;
            uint8_t carry = test.carry;
            sfr::subfze(alias, alias, carry);
            require(alias == test.result && carry == test.next, "source/destination alias uses old source and carry");
        }
        for (uint8_t invalid : {uint8_t(2), uint8_t(255)}) {
            uint64_t destination = 0x12345678;
            uint8_t carry = invalid;
            bool stopped = false;
            try { sfr::subfze(destination, 0, carry); }
            catch (const sfr::RuntimeStop& error) { stopped = error.category == "arithmetic-carry"; }
            require(stopped && destination == 0x12345678 && carry == invalid,
                    "invalid carry stops before any state mutation");
        }
        std::cout << "Integer arithmetic checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
