# Vertex conversion cost measurement

Goal: identify how much uncached CPU vertex conversion remains on the real race
path before selecting a larger optimization for handhelds.

Evidence: Xenia source comparison identifies GPU-side endian/format handling;
SFR already enables its vertex cache, but swap_words_into remains a sampled
hotspot. A second conversion expands packed DEC3N attributes. Their relative
cost and transferred byte counts are not currently separated in frame metrics.

Design: extend opt-in SFR_FRAME_METRICS in guest_graphics_hooks.cpp with cached
source bytes, swapped source bytes, repacked output bytes, and swap/repack time.
No shader, rendering, cache, scheduling or timing behavior changes. Disabled
metrics must not take new clock readings. Counters reset at the existing present
boundary. Validate cached + swapped bytes equals total vertex bytes per frame.

Alternatives: changing the shader ABI now is premature; enabling the existing
cache is redundant. Measuring each conversion stage is the next discriminating
experiment. User has authorized autonomous investigation while away; no extra
design approval is required for this diagnostic step.

Steps:
- Preserve the current executable/map for reproducible before/after measurements.
- Write a real-capture verifier; confirm it rejects the old log missing fields.
- Add counters/timings only to the existing metrics path and build the runtime.
- Run the same isolated race, automatically aborting our own process on overlap.
- Validate all frame accounting and summarize the late window. Pick the next
  optimization from evidence, with fixed game timing and output preserved.
- Record observations, benchmark location and ownership in the shared handoff.

Validation is against actual runtime logs, not tests mirroring counter statements.
These instrumented runs diagnose costs; do not compare their FPS directly to
uninstrumented runs as if the added clocks and logging were free.
