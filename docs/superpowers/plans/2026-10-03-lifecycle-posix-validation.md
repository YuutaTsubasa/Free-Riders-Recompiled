# POSIX lifecycle regression validation

Follow-up to the accepted memory preflight and local lifecycle invalidation repair (8aa640d). At the start of this check, Windows standalone and normal-game checks were complete and POSIX had only been inspected. This check executes existing tests on local WSL Ubuntu x86_64, without contacting devices or modifying gameplay.

## Scope

Use the existing isolated worktree and new ignored out/cpu-lifecycle-posix-v047 artifacts. Freeze current source/header/tests and the retained pre-fix source. Compile with the installed g++ C++20 compiler, preserving compiler/platform/page-size identity, commands, raw logs and source/executable hashes. No dependency installation, tracked runtime changes, user saves, GPU/game, power settings or wake requests. Per-command bounds: compilation 180s, tests 120s plus 5s kill grace, whole helper at most 12min.

## Checks

- [x] Compile current guest-memory regressions against retained pre-fix source and observe exactly the three intended lifecycle dirty-tracking failures.
- [x] Compile current source and pass full guest-memory, physical-memory, virtual-memory suites plus existing 35-case lifecycle oracle, zero stale decisions.
- [x] Independently review requirements and then evidence quality; preserve any failure without broadening production changes.
- [x] Record verification, cleanup and limits in this plan and continuation. No publication or performance claim.

WSL Linux x86_64 with 4096-byte host pages does not establish Android ARM64/16KiB portability, game correctness, graphics output or performance. Existing Windows results are not pooled with this run. No completed gameplay test is repeated.

## Executed evidence

Local WSL Ubuntu 22.04.3, g++ 11.4.0, x86_64-linux-gnu, Linux 6.18.40.1 WSL2, 4096-byte pages. A single CPU-only run completed in 33.031s. The retained pre-fix memory compiled with current guest tests and failed exactly three expected lifecycle dirty assertions plus the aggregate failure (exit 1). The repaired memory passed full applicable guest-memory, physical-memory and virtual-memory suites; the unchanged lifecycle oracle reported 35 cases, zero failures, zero stale decisions. All five compilations succeeded. No timeouts occurred; timeout cleanup branches were not exercised.

Artifacts: out/cpu-lifecycle-posix-v047/run-20261003T061712Z-c41e2bdd. results.json SHA256 a67202221039ef31a0cbbe8a254faf344bb069e5a1f2d98bc2efccd5687f3b64; input-manifest.json e28504501c4846ddfb878a5672a5459b75b1559e6c75763e58977a44258fa70e. Primary independently read raw RED/GREEN logs and verified all 67 artifact hashes. Source and fixture hashes were unchanged during the run; no tracked runtime changes.

Windows-only gates are excluded on this host: VirtualQuery native state/protection checks; placeholder/private-allocation topology and associated destructor assertions; native write-combined mappings and their cache-zero paths. The four added lifecycle regressions are outside those gates and execute the POSIX mmap replacement paths. The 35-case oracle is a test-only retained-data decision model, not a Linux NativeRenderer/GPU test. This closes only the Linux x86_64/4KiB CPU regression gap; it does not establish Android, 16KiB pages, native partial-failure injection, game correctness or performance.


Independent scope and subsequent quality reviews passed. Parent verification confirmed all 67 artifact hashes, all 13 original/frozen input pairs and all 15 expected command exit codes. A fresh Linux PID/start-time check found no surviving owned test/compiler processes; no game, device, wake or recorder was started. Verification is retained in out/cpu-lifecycle-posix-v047/parent-verification.json. No runtime changes, publication or further test rerun.

