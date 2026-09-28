#pragma once
#include <cstdint>
namespace sfr {
// vcmpbfp: compare each float against a bound, one pair of bits per element.
//
// For every element: the top bit says the value is above the bound (not
// a <= b), the next says it is below the negative bound (not a >= -b), and
// zero means it is inside. A NaN on either side is outside both ways, since
// both comparisons are false, which is what the architecture says.
//
// XenonRecomp translates the game's vcmpbfp128 to a debug trap, so the
// function that holds one stops the run (scripts/generate_diagnostic.py
// rewrites the trap into this call). The record form, which also reports in
// CR6 whether every element was inside, is not used by this game and is not
// implemented here.
void vector_compare_bounds(const float (&value)[4], const float (&bound)[4], uint32_t (&result)[4]);
}
