# Android Permit Measurement Implementation Plan

> Execute locally in the current task; preserve the existing input-import changes and TLS commit. The user approved this scope after the report review.

**Goal:** Identify the main thread's actual permit blockers and measure a narrow critical-section wait change on Android.

**Architecture:** Keep per-instance timing under each scheduler's existing mutex. Snapshot ongoing holds at Present, separately for the global permit and six cores. Attribute owner time overlapping guest 1's ready queue separately. Route detached waits through the core permit, preserving guest critical-section coordination and cancellation. An environment switch enables the experimental core-only wait for same-binary A/B; the established attach policy stays the default.

**Tech Stack:** C++20, CMake/CTest, Android NDK 29, adb, Simpleperf.

- [x] Add regression tests for per-instance timing, ongoing holds, attribution while guest 1 queues, and detached waits releasing the core without acquiring the global permit.
- [x] Implement timing snapshots and the tested wait helper in guest_execution; preserve attached-wait behavior.
- [x] Report global/core holds and main-ready blockers from Present; log guest ID / OS TID mappings at thread startup.
- [x] Use the wait helper for critical-section contention; retain SFR_CRITICAL_WAIT_GLOBAL=1 as default and =0 for the experimental comparison.
- [x] Run scheduler/critical-section/memory regression tests on host and device.
- [ ] Build one Android binary and compare both wait policies on the same course. Use bounded windows and record limitations; do not treat a single run as proof of improvement.
- [x] Keep only verified behavior in the device's final configuration and document measurements and remaining bottlenecks.

No general memory-check removal, global-permit removal, shader changes, or unrelated cleanup. GPU wait and renderer sub-timers remain overlapping measurements, not an additive frame budget.

Outcome: same-binary baseline captured; three candidate startup stalls prevented a complete paired A/B. Do not claim a performance gain. Early candidate profile is retained for hotspot identification. Final build restores established wait behavior by default.
