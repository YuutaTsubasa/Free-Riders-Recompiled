#pragma once
// Which game program the launcher starts (Issue #432).
//
// On x86-64 Windows and Linux the release carries the game twice, compiled
// from the same generated code: sfr_cpu_diagnostic for processors with AVX
// (-march=sandybridge) and sfr_cpu_diagnostic_sse for those without
// (-march=x86-64-v2: SSE4.2, SSSE3, POPCNT, CMPXCHG16B). The AVX program on a
// processor without AVX stops at its first AVX instruction (0xC000001D,
// illegal instruction), so the launcher reads CPUID and the operating
// system's register state before it starts one. Players never choose; the
// choice and its reason go into game.log.
//
// SFR_TEST_GAME_PROGRAM (the launcher's environment, for tests only) forces
// one: "sse" or "avx".
#include <cstdint>
#include <string>

namespace sfr {

// What the choice is made from: CPUID leaf 1 and, when the system has
// enabled XSAVE (OSXSAVE), XCR0. On a processor that is not x86-64 nothing
// is read and x86 is false.
struct CpuidBits {
    bool x86 = false;
    uint32_t leaf1_ecx = 0;
    uint64_t xcr0 = 0;
};

namespace cpuid_bit {
inline constexpr uint32_t ssse3 = 1u << 9, cmpxchg16b = 1u << 13, sse4_1 = 1u << 19, sse4_2 = 1u << 20,
                          popcnt = 1u << 23, osxsave = 1u << 27, avx = 1u << 28;
// XCR0: the system saves and restores the SSE (XMM) and AVX (upper YMM) state.
inline constexpr uint64_t xcr0_sse = 1u << 1, xcr0_avx = 1u << 2;
}  // namespace cpuid_bit

// This processor's (x86 false elsewhere).
CpuidBits read_cpuid();

// AVX instructions run here: the processor has them and the system keeps
// their registers across thread switches.
bool avx_usable(const CpuidBits& cpu);
// What the SSE program itself needs (-march=x86-64-v2).
bool sse_build_usable(const CpuidBits& cpu);

enum class GameBuild { standard, sse };

struct GameProgramChoice {
    GameBuild build = GameBuild::standard;
    // For game.log: avx, no-avx, avx-not-enabled-by-system, not-x86,
    // test-override.
    const char* reason = "";
};

// The program for this processor; test_override is SFR_TEST_GAME_PROGRAM
// (null or anything else than "sse" and "avx": no override). Not x86-64:
// always the standard program, the only one built there.
GameProgramChoice choose_game_program(const CpuidBits& cpu, const char* test_override);

// This machine's processor and choice, SFR_TEST_GAME_PROGRAM read from the
// environment; read once.
const CpuidBits& this_cpu();
GameProgramChoice this_machine_game_program();

// The program's file name without the system's extension.
const char* game_program_stem(GameBuild build);

// One line for game.log: LAUNCHER_GAME_PROGRAM program=... reason=... and the
// processor bits it was chosen from.
std::string game_program_log_line(const CpuidBits& cpu, const GameProgramChoice& choice,
                                  const std::string& program_name);

}  // namespace sfr
