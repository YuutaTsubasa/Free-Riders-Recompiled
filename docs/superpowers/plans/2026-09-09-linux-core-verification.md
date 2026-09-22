# Linux runtime core verification plan

**Goal:** Execute the ten existing native runtime tests with a Linux compiler and Linux memory APIs, without claiming a Linux game build.

**Environment:** WSL2 provides Ubuntu 22.04 (no C++ compiler found) and Arch Linux with GCC 14.2.1. Use the existing Arch compiler for the standalone runtime components. No distribution package upgrades or system-service repairs are needed for this verification.

- [x] Compile the ten runtime source files (including guest memory and vector helpers) with `/usr/bin/g++ -std=c++20 -O2 -Isrc` into ignored `out/build/linux-gcc` objects.
- [x] Link and run each existing `tests/*_test.cpp` target corresponding to the ten CTest cases, preserving exact tests and reporting compiler/exit evidence. Keep all outputs within the repository's ignored out directory.
- [x] If a portability failure occurs, diagnose it, reproduce with a focused regression and fix within the existing runtime contract; independently review any production changes.
- [x] Record Linux kernel, compiler and host page size, test results and exact reproduction commands. Distinguish Linux core verification under WSL2 from the full generated CPU diagnostic, graphical game and non-WSL hosts.
- [x] Update progress/toolchain documentation and commit verified source/docs only.
