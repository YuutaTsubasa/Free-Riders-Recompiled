# Android event wakeup implementation plan

**Goal:** Reduce unrelated event wakeups on Android without changing guest timing or synchronization results.

**Evidence:** The Pocket S2 Pro Intro profile attributes 6.38% of sampled CPU to `set_event` broadcasts and 6.03% to portable wait-any futex waits. All portable objects currently share a single condition variable. API 29 native TLS separately improves the measured Intro; preserve that build as the comparison baseline.

**Design:** Keep the shared state mutex and atomic wait-all consumption. Register each blocking wait with its object span and a private condition variable. Signal only registrations containing the changed object, while holding the shared mutex so registrations cannot disappear during notification. Destroy the registration before releasing that mutex on every return/exception. Preserve stop-token handling, deadlines, object ordering, duplicate semaphore counts, and fast polling without registration.

Alternatives: per-object condition variables cannot directly handle wait-any/all; changing notify-all to notify-one globally could wake the wrong waiter and lose progress. A small intrusive list of blocked waits handles both without changing the public API or object ownership.

This follows the user's existing authorization to investigate and improve handheld performance autonomously. No game-clock changes, rendering omissions, or published Android minimum-version changes are included.

- [x] Extend compatibility tests for overlapping object sets, multi-waiter manual events, cancellation, timeout cleanup, and repeated event handoffs; run against the baseline.
- [x] Change `src/portable_waitables.cpp` to targeted registrations; update the header's implementation description.
- [x] Run portable waitable and applicable native synchronization/thread tests. Inspect mutex and stop-callback lifetimes.
- [x] Rebuild Android API 29 and compare the same Intro window against the preserved API 29 baseline, with and without sampling. Keep the change only if correctness checks pass and CPU/frame evidence supports it.
- [x] Package and install the normal APK, verify signature/assets/minimum SDK, and restore the user's original debug settings byte for byte. Record measured results and remaining limits.

Results and limits: `docs/android-performance-2026-10-01.md`. Review found no blocking implementation issue; strengthened blocked-overlap tests with a test-only registration counter and watchdog cancellation to rule out success by timeout recheck. The final normal APK's `.text` and `.rodata` match the measured candidate byte for byte.
