# Performance and internal resolution implementation plan

> Execute the user-approved four steps in the existing isolated worktree. Use
> test-driven changes, independent read-only review and verification before
> completion. Keep the existing Issue #20 fixes and unrelated files intact.

**Goal:** Reduce normal-play CPU overhead, provide repeatable performance
scenarios, and expose real internal render resolution independently of output
window size, without changing the 60 Hz simulation.

**Architecture:** Keep guest graphics coordinates at 1280 x 720; scale native
rendering resources and raster coordinates together. Preserve native 100% as
the default. Diagnostic detail is explicitly enabled for measurement while
ordinary play retains bounded health/crash information. Scenario runs isolate
settings/save/cache and preserve exact configuration metadata.

**Tech stack:** C++20, Plume D3D12/Vulkan, ImGui/launcher settings, Python
unittest, CTest and the existing local game build.

## 1. Measure CPU work

- [x] Sample the validated executable with `SFR_MAIN_PROFILE=1`, fixed 60 FPS,
  warmed cache and saved input; resolve addresses with its matching map file.
- [x] Rank native and recompiled-game work. Separate waiting from active CPU
  work. Do not infer GPU time from the CPU-side fence wrapper.
- [x] Make only evidence-supported changes; benchmark the same scenes and
  preserve pre-change executable/config/logs.

## 2. Remove redundant diagnostics and measured repeated work

Files: `src/guest_graphics_hooks.cpp`, `src/diagnostic_main.cpp`,
`src/input_trace.h`, `tests/input_trace_test.cpp`, optional focused telemetry
helper/test, `scripts/capture_benchmark.ps1`, `docs/benchmarking.md`.

- [x] Add a regression proving quiet input logging records each player's first
  state and connection changes without printing every advancing packet.
  Keep an explicit detailed-input mode for diagnosis.
- [x] Add tested telemetry policy: `SFR_FRAME_METRICS=1` produces consecutive
  detailed frames; normal quiet graphics mode avoids detailed per-draw timing
  and per-frame temporary stream collections, while retaining periodic health
  summaries and errors. Audit all counters before suppressing observation.
- [x] Update both capture tools to explicitly request detailed metrics.
- [x] Remove demonstrably unnecessary diagnostic work, with focused correctness
  tests and gameplay verification. No individual FPS gain is claimed; reject
  speculative scheduler changes without measured benefit.

## 3. Reproducible regression scenarios

Files: `scripts/benchmark_scenarios.py`, `tests/test_benchmark_scenarios.py`,
`docs/benchmark-scenarios.md`.

- [x] Test validation, fresh isolated output, fixed 60 FPS, scenario overrides,
  metadata, preservation of source settings/save and sequential execution.
- [x] Implement guided 1P, 2P, Camera and VRM runs. Require explicit equipment,
  paths and scenario selection. Record required manual actions instead of
  claiming automatic live-camera reproducibility.
- [x] Include dry-run validation and hashes/configuration suitable for another
  device; reject incomplete or incomparable measurements.

## 4. True internal resolution

Files identified by the renderer audit: native presentation/raster/resolve
path, guest shader coordinate setup, launcher settings/UI, relevant renderer
and launcher regression tests. Keep source ownership explicit across agents.

- [x] Trace framebuffer allocation, viewport/scissor, partial loading frames,
  resolved textures, screen-space shader transforms and VRM compositing.
- [x] Use `render_scale` / `SFR_RENDER_SCALE`, integer percent, with native
  100% default and bounded lower/higher presets. Invalid settings fall back to
  native; unsupported resource dimensions must fail clearly.
- [x] Write actual scaled-resource/readback tests before implementation. Check
  native behavior, clipping/clear, split viewport coverage and sampling a
  resolved image on both graphics backends.
- [x] Expose the setting separately from output-window resolution, with a
  clear restart requirement and pixel dimensions.
- [x] Capture 1P menu/loading/HUD/gameplay at low/native/high scales. Confirm
  rendering resolution changes, not just screenshot/window size. Split viewports
  and VRM depth/HUD ordering pass focused GPU readback regressions.
- [ ] Human 2P/Camera/VRM gameplay and affected low-end-device comparisons remain
  external validation; the new guided runner records these separately.

## Integration and delivery

- [x] No overlapping games, builds or heavy tests during performance windows.
- [x] Run targeted red/green tests, full CTest/Python checks, independent code
  review, and final game verification with both backends.
- [x] Update README/benchmark instructions, actual measurements and limitations.
- [x] Update the separate local test package, verify hashes and preserve player
  settings/save. No release or remote publication was requested.

Final evidence and limitations: [Validation report](../../performance-resolution-validation.md).
