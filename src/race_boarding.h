#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace sfr {
// 82370588 advances its path by elapsed frames, but emits one frame of
// boarding displacement. Correct only that emitted displacement after the
// original has updated its direction blend and before 8236FB80 handles 349.
// Memory is the guest's endian-aware load/store interface (also used by tests).
template<class Memory>
bool correct_boarding_step(Memory& memory, uint32_t object, uint32_t first,
                           uint32_t second, float frames, uint8_t entry_phase) {
    if (frames == 1.0f || !std::isfinite(frames) || frames <= 0.0f ||
        entry_phase != 1 || memory.template load<uint8_t>(object + 500) != 1 ||
        memory.template load<uint32_t>(object + 336) != 1 ||
        memory.template load<uint32_t>(object + 340) != 1 ||
        memory.template load<uint8_t>(object + 349) != 0)
        return false;
    const auto read = [&](uint32_t address) {
        return std::bit_cast<float>(memory.template load<uint32_t>(address));
    };
    std::array<double, 3> step{};
    double travel_squared = 0, distance_squared = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        const double position = read(first + 288 + i * 4);
        step[i] = (double(read(first + 368 + i * 4)) - position) * frames;
        const double distance = double(read(object + 32 + i * 4)) - position;
        travel_squared += step[i] * step[i];
        distance_squared += distance * distance;
    }
    if (!std::isfinite(travel_squared) || !std::isfinite(distance_squared))
        return false;
    if (distance_squared <= travel_squared) {
        // The original reach branch copies all four components to both riders.
        // Leave current state 336 alone: its caller must run the exit action.
        for (const uint32_t rider : {first, second}) {
            for (uint32_t i = 0; i < 16; i += 4) {
                const auto word = memory.template load<uint32_t>(object + 32 + i);
                memory.template store<uint32_t>(rider + 288 + i, word);
                memory.template store<uint32_t>(rider + 368 + i, word);
            }
        }
        memory.template store<uint32_t>(object + 340, 2);
        memory.template store<uint8_t>(object + 349, 1);
    } else {
        for (const uint32_t rider : {first, second}) {
            for (uint32_t i = 0; i < 3; ++i) {
                const float next = float(double(read(rider + 288 + i * 4)) + step[i]);
                memory.template store<uint32_t>(rider + 368 + i * 4,
                                                std::bit_cast<uint32_t>(next));
            }
            if (first == second) break;
        }
    }
    return true;
}
}
