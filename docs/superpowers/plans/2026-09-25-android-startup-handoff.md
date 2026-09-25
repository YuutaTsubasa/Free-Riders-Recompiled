# Android startup completion handoff

Scope: continue the user-authorized startup diagnosis and performance work; preserve prior round3 and input changes. No blanket scheduler or memory-check removal.

Evidence: `out/android-startup-round4/trace1.log` lines 19976–19994 show guest 35 signals event 0x72100200, main completes that wait and resumes handle 0x72200088, main waits again, then guest 35 finally self-suspends. Main wait back chain reaches 0x824C3A98, worker is 0x824C39C8. An early ResumeThread at zero count cannot wake a later SuspendThread.

Design: only the completion call at LR 0x824C3A3C prepares its self-suspension before SetEvent. The following self-suspend consumes that one registration; ordinary and foreign suspensions still increment normally. The global permit serializes registration/resume, and the native wait captures its stable record before releasing that permit. Its count is atomic so polling does not race with resume or traverse a changing registry. A diagnostic override disables the handoff for comparison. Do not change general ResumeThread into a counting notification.

- [x] Capture import arguments, wait handles and main back chain using an opt-in trace.
- [x] Reproduce the early-resume failure in a deterministic host test (failed with old no-op registration).
- [x] Implement narrow prepare/consume handoff and test ordinary waits, late resume, nested foreign suspension, bad outputs and cancellation.
- [x] Build and run host regression tests and Android guest_threads test.
- [x] Run repeated device startups, including trace disabled, then enter a race.
- [x] Restore ordinary device settings; document exact results and remaining limitations.

Trace and test data are in `out/android-startup-round4`. Android CMake cache now uses SDK Ninja instead of MSYS Ninja: full rebuild succeeded, subsequent runtime/test edit rebuilt in about 8 seconds with no corrupt-dependency warning. This changes local tooling only.

Device outcome: three consecutive startups reached title (trace on/off; normal/skip movies); third ran 906 race frames without HANG_REPORT or RUNTIME_STOP. Measured windows 14.68 / 11.40 FPS, not a demonstrated performance increase. Installed verified APK and restored original debug.env; game left paused.
