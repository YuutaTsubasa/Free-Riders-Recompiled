# Source intake and CPU recompilation implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task by task.

**Goal:** Reproducibly inspect/extract the supplied Free Riders disc, build pinned Xenon tools, generate actual game C++, and execute its entry point with explicit diagnostics for missing runtime services.

**Architecture:** Python standard-library source tools validate the disc and XEX without shipping game bytes. Pinned upstream C++ tools handle XEX decryption and PowerPC translation. A separate native diagnostic executable provides guest memory and reports unsupported calls instead of pretending to run the game.

**Tech stack:** Python 3.12, unittest, CMake/Ninja, Clang 21, XenonRecomp/XenonAnalyse.

The complete five-platform objective remains in the approved design. This plan covers the dependent M0/M1 work package; findings determine the detailed M2 plan. The initial directory has no repository or concurrent edits to isolate, so initialize a development branch in place; keep ROM/private/generated/upstream build files excluded.

## Task 1: Disc reader and asset intake — completed

Files: `scripts/freeriders/{__init__,disc,xex}.py`, `scripts/rom_tool.py`, `tests/test_disc.py`, `tests/test_xex.py`, `config/source.json`.

- [x] Write synthetic-image tests for valid nested XDVDFS, bounds, duplicate/case-colliding names, traversal names, cyclic directory references, truncation, extraction refusal into existing destinations, and XEX metadata bounds.
- [x] Run `python -m unittest discover -s tests -v`; observe missing functionality fail.
- [x] Implement bounded reads, validated directory traversal, staged extraction to a new directory and atomic manifest completion, with SHA-256 per file. Validate the expected XEX digest/Title ID for supported extraction. Provide `inspect`, `extract`, and `xex` CLI commands; keep images read-only.
- [x] Run the tests again and run `python scripts/rom_tool.py inspect --iso <local ISO> --output out/source-report.json` against the supplied image.
- [x] Extract at least `default.xex` to `private/game` and compare its digest with `3d58636bec9b92948f7dbe423c0aa96753349ba014194c4e62d2f809c14643a7`.

## Task 2: Pinned upstream tools — completed

Files: `scripts/build_tools.ps1`, `config/dependencies.lock.json`, `docs/toolchain.md`.

- [x] Clone public XenonRecomp with submodules into ignored `tools/XenonRecomp`; record exact commit and submodule status.
- [x] Read upstream CMake and configure Clang with the detected Visual Studio SDK using explicit tool paths. Build XenonAnalyse and XenonRecomp. Preserve command output in `out`.
- [x] Record repeatable configure/build commands and verify the built tools execute; document any local compatibility change as a tracked patch rather than an invisible vendored edit.

## Task 3: Free Riders CPU analysis — partial; semantic gaps recorded

Files: `scripts/analyse_game.py`, `config/freeriders.toml`, `config/dependencies.lock.json`, generated artifacts under ignored `generated`/`out`.

- [x] Run XenonAnalyse against the verified local XEX; preserve analysis diagnostics and switch tables.
- [ ] Inspect decoded executable code/sections and identify register save/restore functions from actual byte patterns and instruction sequences. Reject ambiguous matches. Identify setjmp/longjmp or explicitly record that they remain unresolved.
- [x] Add synthetic pattern/bounds tests before writing analysis helpers. Disable speculative optimizations in the game configuration.
- [x] Run XenonRecomp, preserve all diagnostics, count produced functions and unresolved cases, and compile actual generated translation units.

## Task 4: Diagnostic entry execution — entry stop verified; full runtime remains

Files: `CMakeLists.txt`, `src/diagnostic_main.cpp`, `src/guest_memory.*`, `scripts/generate_import_diagnostics.py`, tests for guest memory and import mapping.

- [x] Read upstream generated ABI and image loading requirements before implementing the host.
- [x] Test bounds and big-endian initialization with small synthetic guest-memory fixtures.
- [x] Build guest function table and imports from the verified generated output; initialize stack/TLS only from confirmed ABI/metadata.
- [x] Execute the actual `0x824D22F0` function with a bounded timeout. Missing imports must print name/address and return a nonzero status. Capture the first execution trace; distinguish this milestone from reaching menus or gameplay.

## Task 5: Review and evidence

Files: `README.md`, `docs/progress.md`, relevant tests and this checklist.

- [x] Run full source-tool tests, real-disc verification, clean native configure/build, and diagnostic execution where preceding tasks permit it.
- [x] Review implementation against the approved design, then review error handling and cross-platform assumptions; fix actionable issues.
- [x] Record exact achieved milestones, tool versions, commands, counts, failures and next runtime work. Never mark M1 complete unless the actual entry executes.
- [x] Verify `git status --short` and `git check-ignore` keep ROM, private assets and generated output out of source control. Commit only project code/config/docs after verification.

## Verified checkpoint

M0 complete for the supplied disc. M1 partial: diagnostic C++ builds and the actual entry reaches the unimplemented XexExecutableModuleHandle variable after four function entries. Full raw translation is not correct yet (16,953 diagnostic function stops); no playable game or non-Windows build is claimed. See docs/progress.md for 76-test and native execution evidence, including one preceding Windows rename failure. This checkpoint does not complete the approved five-platform objective.
