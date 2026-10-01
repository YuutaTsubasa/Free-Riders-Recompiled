# Android phone diagnostics implementation plan

**Goal:** Let a player without USB access save useful performance evidence from the Android launcher.

**Design:** Add an Android-only Export diagnostic ZIP button in Game files. Android's document creation picker chooses the destination; ZIP compression runs off the UI thread. Explicitly include a bounded game log, selected numeric performance settings from the last launch, and app/device information. Missing logs are reported in the archive. Cancellation and write failures remain recoverable. Do not include game data, saves, models, or images.

**Architecture:** A platform-independent Java archive writer handles file selection and size limits. LauncherActivity handles the picker and status, called through the existing SDL JNI bridge. Quiet native frame logging summarizes existing frame counters without enabling per-draw profiling. Frame rate means guest present-hook rate, not Android display refresh rate.

**Validation:** JVM tests exercise the real ZIP writer (missing files, bounded large logs, settings filtering and I/O failure). Compile Android native targets, package and verify a signed Android 10+ diagnostic APK, and inspect the export UI on an idle test device if available. S25 FE performance remains unverified until the user provides recordings.

- [x] Test and implement bounded archive writer.
- [x] Add document picker integration and bilingual launcher button.
- [x] Add low-cost interval timing summary using existing measurements.
- [x] Build, verify archive/APK, review changes and provide same-race 720p/360p collection instructions.

Validation: JVM archive tests passed (small/missing log, large-log head/tail bound, settings allowlist, output failure). Android API 29 native targets built; signed APK and shader/library bytes verified. AYN Thor saved a valid ZIP through the system picker. A bounded 360-frame native smoke run emitted two 120-frame summaries whose FPS/interval/max/mean relationships passed assertions. The private test override was removed afterwards. Independent review found no blockers. S25 FE race data remains pending.

Files: `android/app/src/main/java/com/freeriders/recompiled/DiagnosticsArchive.java`, `LauncherActivity.java`, `src/launcher_main.cpp`, `src/launcher_platform.h`, `src/launcher_platform_sdl.cpp`, `src/guest_graphics_hooks.cpp`, `tests/test_android_diagnostics.py`, `tests/java/DiagnosticsArchiveTest.java`.
