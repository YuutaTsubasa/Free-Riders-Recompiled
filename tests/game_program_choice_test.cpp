// Issue #432: the launcher starts the AVX game only where AVX runs, and the
// SSE game everywhere else on x86-64 (src/game_program_choice.h), from fake
// CPUID results.
#include "game_program_choice.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using namespace sfr::cpuid_bit;
constexpr uint32_t nehalem = ssse3 | cmpxchg16b | sse4_1 | sse4_2 | popcnt;  // i7-740QM, the issue's laptop
constexpr uint32_t sandy_bridge = nehalem | osxsave | avx;
constexpr uint64_t os_avx = xcr0_sse | xcr0_avx | 1;  // x87 | SSE | AVX, as Windows 10 sets it

sfr::CpuidBits cpu(uint32_t ecx, uint64_t xcr0) { return {true, ecx, xcr0}; }

bool picks(const sfr::CpuidBits& bits, sfr::GameBuild build, const char* reason, const char* test_override = nullptr) {
    const auto choice = sfr::choose_game_program(bits, test_override);
    return choice.build == build && std::strcmp(choice.reason, reason) == 0;
}
}  // namespace

int main() {
    using sfr::GameBuild;
    // A processor with AVX that the system saves: the AVX game.
    check(picks(cpu(sandy_bridge, os_avx), GameBuild::standard, "avx"), "AVX processor and system: AVX game");
    check(sfr::avx_usable(cpu(sandy_bridge, os_avx)), "AVX usable");
    // The issue's i7-740QM (Nehalem): SSE4.2 and no AVX.
    check(picks(cpu(nehalem, 0), GameBuild::sse, "no-avx"), "Nehalem: SSE game");
    check(sfr::sse_build_usable(cpu(nehalem, 0)), "Nehalem runs the SSE game");
    // Nehalem with XSAVE enabled by the system but no AVX (OSXSAVE alone).
    check(picks(cpu(nehalem | osxsave, xcr0_sse | 1), GameBuild::sse, "no-avx"), "OSXSAVE without AVX: SSE game");
    // AVX in the processor, but the system does not enable XSAVE (Windows 7
    // without SP1), or enables it without the AVX state.
    check(picks(cpu(nehalem | avx, 0), GameBuild::sse, "avx-not-enabled-by-system"), "AVX without OSXSAVE: SSE game");
    check(picks(cpu(sandy_bridge, xcr0_sse | 1), GameBuild::sse, "avx-not-enabled-by-system"),
          "AVX without the YMM state in XCR0: SSE game");
    check(picks(cpu(sandy_bridge, xcr0_avx | 1), GameBuild::sse, "avx-not-enabled-by-system"),
          "AVX without the XMM state in XCR0: SSE game");
    // Not x86-64 (ARM64 Linux, Android, macOS): the one program built there.
    check(picks(sfr::CpuidBits{}, GameBuild::standard, "not-x86"), "not x86: standard game");
    check(picks(sfr::CpuidBits{}, GameBuild::standard, "not-x86", "sse"), "not x86: the override does not apply");
    // The test override, both ways, and values it does not know.
    check(picks(cpu(sandy_bridge, os_avx), GameBuild::sse, "test-override", "sse"), "override sse on an AVX PC");
    check(picks(cpu(nehalem, 0), GameBuild::standard, "test-override", "avx"), "override avx on Nehalem");
    check(picks(cpu(sandy_bridge, os_avx), GameBuild::standard, "avx", ""), "empty override is none");
    check(picks(cpu(nehalem, 0), GameBuild::sse, "no-avx", "AVX"), "override is exact");
    // What the SSE game needs: an older processor without SSE4.2 (Core 2,
    // Phenom II) cannot run it either; the log says so.
    check(!sfr::sse_build_usable(cpu(nehalem & ~sse4_2, 0)), "no SSE4.2: SSE game not usable");
    check(!sfr::sse_build_usable(cpu(nehalem & ~popcnt, 0)), "no POPCNT: SSE game not usable");
    // The names and the log line.
    check(std::strcmp(sfr::game_program_stem(GameBuild::standard), "sfr_cpu_diagnostic") == 0, "standard name");
    check(std::strcmp(sfr::game_program_stem(GameBuild::sse), "sfr_cpu_diagnostic_sse") == 0, "sse name");
    const std::string line = sfr::game_program_log_line(cpu(nehalem, 0), sfr::choose_game_program(cpu(nehalem, 0), nullptr),
                                                        "sfr_cpu_diagnostic_sse.exe");
    check(line == "LAUNCHER_GAME_PROGRAM program=sfr_cpu_diagnostic_sse.exe reason=no-avx cpu_avx=0 osxsave=0 "
                  "os_avx_state=0 sse4_2=1 popcnt=1 sse_build_ready=1\n",
          "log line");
    // This machine: whatever it is, the choice is one of the two, and an x86
    // processor that reports AVX usable picks the AVX game.
    const auto here = sfr::read_cpuid();
    const auto chosen = sfr::choose_game_program(here, nullptr);
    std::printf("%s", sfr::game_program_log_line(here, chosen, sfr::game_program_stem(chosen.build)).c_str());
    check(chosen.build == (here.x86 && !sfr::avx_usable(here) ? GameBuild::sse : GameBuild::standard), "this machine");
    if (failures) return 1;
    std::printf("game program choice: all checks passed\n");
    return 0;
}
