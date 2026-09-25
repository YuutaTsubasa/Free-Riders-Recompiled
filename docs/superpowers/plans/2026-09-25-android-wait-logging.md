# Android wait-result logging experiment

Continue authorized performance work inline using systematic debugging. Preserve previous fixes and the warmed Vulkan pipeline cache.

Evidence: round5 warm.log's 200 measured frames contain 4,771 log lines, including 3,852 CriticalSectionWait and 200 NtWaitForMultipleObjectsEx result lines. These two sites ignore SFR_TRACE_IMPORTS=0, unlike neighboring wait/import traces.

Design: let these successful result lines inherit trace_imports. Keep unsuccessful NT wait results visible. SFR_TRACE_WAIT_RESULTS=1 restores only the old two sites and =0 disables them, allowing same-APK A/B with no scheduler or wait changes. This reversible logging guard does not need an implementation-mirroring unit test; device output counts and repeated timing windows verify its effect.

- [x] Count measured-frame log sources and inspect actual wait/trace code.
- [x] Add the two logging guards and isolated diagnostic override.
- [x] Build/package one Android APK; verify the guard is the only runtime delta from round5.
- [x] Compare repeated warm-cache races with logging on/off. Check frame continuity, draw counts, pipeline times, temperatures and emitted result counts.
- [x] If improvement is within noise, report it as such and obtain current per-thread CPU evidence for the next bottleneck.
- [x] Restore normal settings and document the measured outcome; no commit requested.

Outcome: ~87% fewer measured log lines; no reproducible FPS gain. Four on/off/off/on races completed. Fresh per-thread CPU profile and CAS attribution saved; normal APK installed, original debug.env restored byte-exact, warmed cache loaded. Detailed results in docs/performance.md (round6).
