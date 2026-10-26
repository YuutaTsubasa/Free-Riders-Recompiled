# PGO profiles

`android-arm64.profdata.gz` is the profile-guided optimisation profile that
Android arm64 builds of the game use by default (`SFR_PGO=auto` in
CMakeLists.txt; `-DSFR_PGO=off` or `scripts/build_android.sh --pgo off`
builds without it). It is an LLVM IR profile merged with `--sparse`
(functions that never ran are left out and compiled normally), gzipped;
CMake unpacks it into the build directory. Measurements:
docs/performance.md, "Compiler options for the Android game library".

## This profile

- Source: commit 0ec510c (main at a64f2da, after v0.10.35's changes, plus
  the SFR_PGO changes), the
  release's generated code (`out/recomp-midasm/diagnostic`, the default
  `SFR_REGISTER_LOCALS`, loop-head checkpoints on), NDK 29
  (clang 20, r547379), API 29, Release.
- Collected on the AYN Thor (Snapdragon 8 Gen 2), 2026-10-26, three runs of
  the instrumented APK, all cores, through the boot, the launcher and the
  menus each time:
  1. Dolphin Resort, Free Race standard, pushing off every 1.2 s, to present
     21,000 (the race from ~13,000);
  2. Metropolis Speedway, the same;
  3. the title left idle to present 9,000 with movies on (the attract movie
     plays at ~6,200).
- `llvm-profdata merge --sparse` of the three `.profraw` files: 2.6 MB,
  0.5 MB gzipped.

## When to collect it again

A function whose code changed since the profile was collected loses its
profile (it is compiled as without one). The build says so: Clang warns
`function control flow change detected (hash mismatch)` for each one, and
after libmain.so links the build prints the total (`SFR_PGO: N function(s)
in M file(s) changed since the profile was collected`). Functions that are
new, or that the profile never saw, are not reported.

Collect it again when the generated code changes (a new XenonRecomp or
generator change, `scripts/register_locals.py`,
`scripts/loop_head_checkpoints.py`, the `SFR_REGISTER_LOCALS` or checkpoint
settings), when the runtime's hot paths change a lot, or when the warning's
count grows; and before a release where it reports more than a handful.

## How to collect it

1. Build an instrumented game from the same source and generated code as the
   release: `scripts/build_android.sh --abi arm64-v8a --pgo generate` (or
   `-DSFR_PGO=generate` on a CMake build directory). The stripped
   libmain.so is ~75 MB instead of ~47 MB and runs slower, but holds 60 fps
   on a fast phone.
2. Install it and play: a race on each of the two courses above to the end
   of the run, and the title left idle until the attract movie has played.
   The benchmark's `SFR_PRESENT_LIMIT` ends a run cleanly. When the game
   exits, `android_main.cpp` writes the counters as
   `default_<id>_0.profraw` into the app's files directory
   (`/sdcard/Android/data/com.freeriders.recompiled/files`); pull each one
   with adb and delete it from the phone before the next run (a later run
   would otherwise merge into the same file).
3. Merge with the NDK's llvm-profdata and compress:

       llvm-profdata merge --sparse -o android-arm64.profdata dolphin.profraw metro.profraw title.profraw
       gzip -9 -n android-arm64.profdata

4. Replace `android-arm64.profdata.gz` here and update "This profile" above.
   Build directories pick the new file up by themselves (it is a configure
   dependency, and its hash is part of every compile command).
