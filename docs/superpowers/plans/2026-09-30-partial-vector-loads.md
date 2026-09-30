# Partial vector-load fast path

Goal: remove redundant per-byte page checks in ordinary-memory lvlx/lvrx loads,
without changing selected byte ranges, padding, providers, faults or timing.

Evidence: the corrected Intro guest-32 sample attributes 4.53% and 2.59% of
19,706 samples to the checked left/right helpers (plus 4.45% in their wrappers).
These percentages include idle sampling in the denominator and are not FPS.
Unlike full-vector loads, partial loads unconditionally preflight and then call
GuestMemory::load once per byte. They stay within a single aligned 16-byte block.

Design: ask existing GuestMemory::fast_read for the exact selected range. If
eligible, perform the same ordered volatile byte reads through that pointer,
with zero padding. Otherwise retain the exact checked implementation, including
per-byte computed-provider sampling. Aligned right loads still access nothing.
Do not alter diagnostic_main.cpp (Claude's area), memory policy or generated code.

Validation:
- Preserve the current runtime/map and measure an isolated baseline against
  the original helper for all offsets before editing.
- Keep the existing independent byte/bounds/provider/reservation tests passing.
- Build Windows and Android ARM64; repeat the helper benchmark with output checks.
- Profile the same corrected Intro segment, then run full playback. Treat a
  decoder-cost improvement separately from capped video FPS and handheld proof.

Alternatives: bypassing memory guards globally is unacceptable; regenerating
the full recompiler to inline helpers is much broader. Reuse the existing
eligible-page API as the narrow experiment first.

Result: retained as an 11-line source change after byte/bounds/provider tests,
Windows runtime build, Android ARM64 test build and independent review.
Warm helper ratios: left 0.4072, right 0.4917. Actual decoder profile aggregate
for partial loads and wrappers: 11.55% to 8.71% of sampled observations.
Full post-edit movie duration 72.476 s versus 72.4824 s before; no timing policy
change. These measurements do not establish handheld FPS or exact A/V sync.
