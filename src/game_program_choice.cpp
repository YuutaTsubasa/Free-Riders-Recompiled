#include "game_program_choice.h"

#include <cstdlib>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#define SFR_CPUID_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace sfr {

CpuidBits read_cpuid() {
    CpuidBits cpu;
#ifdef SFR_CPUID_X86
    cpu.x86 = true;
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 0);
    if (registers[0] < 1) return cpu;
    __cpuid(registers, 1);
    cpu.leaf1_ecx = uint32_t(registers[2]);
#else
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return cpu;
    cpu.leaf1_ecx = ecx;
#endif
    // XGETBV faults unless the system enabled XSAVE (OSXSAVE). Written as
    // its bytes: Clang's _xgetbv needs the whole function built for XSAVE.
    if (cpu.leaf1_ecx & cpuid_bit::osxsave) {
#if defined(_MSC_VER) && !defined(__clang__)
        cpu.xcr0 = _xgetbv(0);
#else
        uint32_t low = 0, high = 0;
        __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(low), "=d"(high) : "c"(0));
        cpu.xcr0 = (uint64_t(high) << 32) | low;
#endif
    }
#endif
    return cpu;
}

bool avx_usable(const CpuidBits& cpu) {
    using namespace cpuid_bit;
    const uint64_t state = xcr0_sse | xcr0_avx;
    return cpu.x86 && (cpu.leaf1_ecx & avx) && (cpu.leaf1_ecx & osxsave) && (cpu.xcr0 & state) == state;
}

bool sse_build_usable(const CpuidBits& cpu) {
    using namespace cpuid_bit;
    const uint32_t needed = ssse3 | cmpxchg16b | sse4_1 | sse4_2 | popcnt;
    return cpu.x86 && (cpu.leaf1_ecx & needed) == needed;
}

GameProgramChoice choose_game_program(const CpuidBits& cpu, const char* test_override) {
    if (!cpu.x86) return {GameBuild::standard, "not-x86"};
    if (test_override && std::strcmp(test_override, "sse") == 0) return {GameBuild::sse, "test-override"};
    if (test_override && std::strcmp(test_override, "avx") == 0) return {GameBuild::standard, "test-override"};
    if (avx_usable(cpu)) return {GameBuild::standard, "avx"};
    // The processor has AVX but the system does not save its registers
    // (an old Windows 7 without SP1, or AVX switched off): the same as none.
    if (cpu.leaf1_ecx & cpuid_bit::avx) return {GameBuild::sse, "avx-not-enabled-by-system"};
    return {GameBuild::sse, "no-avx"};
}

const CpuidBits& this_cpu() {
    static const CpuidBits cpu = read_cpuid();
    return cpu;
}

GameProgramChoice this_machine_game_program() {
    return choose_game_program(this_cpu(), std::getenv("SFR_TEST_GAME_PROGRAM"));
}

const char* game_program_stem(GameBuild build) {
    return build == GameBuild::sse ? "sfr_cpu_diagnostic_sse" : "sfr_cpu_diagnostic";
}

std::string game_program_log_line(const CpuidBits& cpu, const GameProgramChoice& choice,
                                  const std::string& program_name) {
    using namespace cpuid_bit;
    std::string line = "LAUNCHER_GAME_PROGRAM program=" + program_name + " reason=" + choice.reason;
    if (cpu.x86) {
        const auto bit = [&](uint32_t mask) { return (cpu.leaf1_ecx & mask) ? "1" : "0"; };
        line += std::string(" cpu_avx=") + bit(avx) + " osxsave=" + bit(osxsave) +
                " os_avx_state=" + (((cpu.xcr0 & (xcr0_sse | xcr0_avx)) == (xcr0_sse | xcr0_avx)) ? "1" : "0") +
                " sse4_2=" + bit(sse4_2) + " popcnt=" + bit(popcnt) +
                " sse_build_ready=" + (sse_build_usable(cpu) ? "1" : "0");
    }
    return line + "\n";
}

}  // namespace sfr
