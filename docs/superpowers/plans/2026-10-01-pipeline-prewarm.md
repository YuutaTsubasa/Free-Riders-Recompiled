# Pipeline startup preparation implementation plan

> Execute in the existing isolated checkout on `codex/pipeline-prewarm`. Use subagent-driven-development for isolated components and review the integrated implementation before delivery.

**Goal:** Before the first game frame, prepare known graphics pipelines from a portable manifest with visible progress and cancellation. Record newly encountered recipes for later starts and distribution. Never skip a guest draw.

**Architecture:** A versioned, bounded manifest stores shader source identities and graphics state, never pointers or driver binaries. Resolve shaders only from the matching packaged shader inventory. During initial graphics-device setup a worker creates pipelines while the presentation thread pumps events and displays progress. Reuse the prepared pipelines in normal rendering via stable recipe keys. Unknown recipes retain synchronous rendering. Merge a shipped manifest with local learned recipes; persist atomically. Missing or stale optional recipes are ignored with diagnostics; invalid shader packs keep their existing actionable failure.

**Tech stack:** Existing C++20/Plume/Vulkan/D3D12, native Win32/SDL presentation, CMake/CTest, PowerShell packaging.

## Tasks

- [x] Portable recipe codec: add `src/pipeline_manifest.{h,cpp}` and `tests/pipeline_manifest_test.cpp`. Store backend, shader ABI, source hash+size for vertex/pixel stages, specialization values, input layout and all existing pipeline state. Bound file/record/element counts and validate enums before driver calls. Round-trip, dedupe, mismatch, corrupt/truncated, invalid state and address independence tests must fail before implementation.
- [x] Preparation UI: add `NativePresentation::preparation_progress(size_t completed, size_t total, bool cancelling=false)` and `finish_preparation()` in native_presentation. Draw readable progress in the framebuffer, keep events live, support close/Escape and Android Back; do not advance guest presents. Compile on Windows and Android. Cancellation stops scheduling further pipelines; an in-flight driver call must finish before resources are released.
- [x] Shader resolution: expose identity-based packaged lookup in runtime_shader_cache; reject ambiguous/missing IDs, preserve pack ABI validation, do not translate missing manifest entries. Add source entry metadata to NativeDraw, set it in guest_graphics_hooks.
- [x] Renderer: factor one pipeline descriptor construction path used by prewarm and draws. Stable manifest keys must identify equivalent prepared pipelines even when shader object addresses differ. Own prepared shader objects until renderer teardown. Record only serializable guest recipes, write learned manifest at safe frame boundaries/teardown, and retain normal fallback drawing.
- [x] Startup coordinator: invoke preparation after initial presentation/shader setup and before guest device creation returns. Worker completion and failures are synchronized; UI continues at a bounded refresh rate. Log counts, timing, skipped entries, cancellations and first-use misses. No game clock or FPS-cap changes.
- [x] Integration: add build and Windows CI targets; bundle manifest in Windows and Android packages and configure paths. Add readable usage/coverage notes. Capture an initial real-game manifest using the existing owned-game benchmark, then repeat cold and warm cache runs with it. Keep the baseline package intact.
- [x] Verification: codec/unit tests; rendering readback regression on both Windows backends; manifest-record then prewarm reuse counts; first launch with empty GPU cache; repeat with GPU cache; cancel during preparation; missing/corrupt/stale manifests; Android build. Review implementation and package a private candidate with hashes and measured limitations.

## Acceptance and limits

The preparation stage must render progress while compiling and preserve the first game frame's rendering state. A prewarmed recipe must not be compiled again when first drawn. CPU/GPU-dependent driver cache data stays local. The initial recorded route is coverage evidence, not a promise to cover every course, item, character, 2P or custom avatar. UI says preparing pipelines, not that all possible shaders are known. No public release or PR merge is authorized by this implementation request.

## Verification results (2026-10-01)

- 12 targeted CTest regressions passed: both GPU backends, pipeline manifests,
  resolution/rendering, shader-pack failures, pipeline keys and cache files.
- 7 Python packaging/shader-pack tests passed. Windows and Android builds passed.
- GPU readback tests compare preparation on/off, independent shader object
  addresses, changed write masks, corrupt/missing packs/lists and 4096 stale
  recipes. The full-size progress screen is exercised on both backends.
- D3D12 full-size progress originally reproduced an NVIDIA driver stack-cookie
  failure with hundreds of clear rectangles; batches of 16 eliminated it.
- Real Vulkan capture: 279 recipes prepared; 11,500 frames reused 263 known
  recipes and created one additional pipeline (0.0282 ms). This is coverage
  evidence on this desktop, not a handheld speedup claim.
- Empty application-cache startup prepared 279 recipes in 271.647 ms; driver-
  internal caches were not cleared. A later warm-cache run prepared 280 in
  51.285 ms. Intro played through to the title screen. No FPS/clock policy changed.
- Actual D3D12 capture and preparation passed (280 recipes, 802.794 ms); race
  screenshots and startup/Intro smoke were checked. Final package includes
  283 Vulkan / 280 D3D12 recipes, and an extracted-package launch prepared
  all 283 Vulkan recipes with zero skipped entries.
- Cancellation fixture verifies queued close before compilation. Worker join
  and in-flight driver cancellation safety were reviewed, not fault-injected.
- Windows ZIP contents and executable hashes verified. Android APK signature,
  alignment, native library, shader pack and manifest verified. No Android
  device execution or new Ally X performance result is claimed.
- Independent spec and quality reviews completed; identified cache-handle and
  stale-capacity issues were fixed before packaging.