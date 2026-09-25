# Android pipeline-key construction implementation plan

Continue the authorized renderer performance work inline. Use systematic-debugging, writing-plans, executing-plans and differential regression tests. Preserve all existing changes and the warmed disk pipeline cache; no commit or checkout change requested.

**Goal:** Measure whether replacing repeated vector insertions in every draw's pipeline key reduces steady CPU cost without changing any key bytes or draw state.

**Design:** Keep the existing map, key representation, field ordering and raw object representations, including blend/stencil padding. Allocate the vector's required logical size once, then use fixed-size memcpy writes into it. Input is always the current NativeDraw, so there is no guest-memory cache or stale declaration invalidation. Keep the original serializer as a same-binary diagnostic fallback, selected by SFR_PIPELINE_KEY_BULK=0. The optimized path is the default after verification. Optional SFR_PIPELINE_KEY_VERIFY=1 builds both keys on each device draw and rejects mismatches. Verification is disabled for timed runs.

Alternatives: caching declarations could avoid more work but requires tracking writes; a typed key changes key identity and padding/equality semantics. Start with byte-exact construction to isolate a directly observed cost (byte-vector insertion is 1.70% of main CPU samples, not the whole 27% native_draw subtree). No assumed FPS gain.

- [x] Add src/native_pipeline_key.h declarations and a differential regression in tests/native_pipeline_key_test.cpp; wire CMake target native_pipeline_key. Start with an empty optimized serializer and verify the test fails on a nonempty key.
- [x] Move original serialization unchanged into a legacy helper. Implement one-resize bulk construction. Differential cases: zero / 1 / 20 / >20 elements, changing/shrinking reused buffers, stencil on/off, each keyed state mutation, non-key draw data unchanged, varied object padding. Run host and Android tests; optional microbenchmark compares the same keys without treating timing as a unit assertion.
- [x] Integrate only the serializer selection into src/native_renderer.cpp, with opt-in dual verification. Build/package a normal Android APK; preserve shader pack identity. Run a verify-on race before performance timings.
- [x] Use one APK for four warm-cache races (legacy / bulk / bulk / legacy), verify continuous frames, draw medians, pipeline creation cost, temperature and absence of runtime stop. No input during race; no profiler/dual verification during timing.
- [x] Restore original device debug.env, preserve cache, record measured outcome in docs/performance.md, and identify whether FPS change is repeatable. Keep no performance claim without evidence.

Validation commands: cmake --build out/build/host --target sfr_native_pipeline_key_test; ctest --test-dir out/build/host -R ^native_pipeline_key$ --output-on-failure; cmake --build out/build/android-tls29 --target sfr_native_pipeline_key_test sfr_cpu_diagnostic -j16. Push the Android test to /data/local/tmp and execute it. Package using scripts/package_android.py --abi arm64-v8a --min-sdk 29 --pack out/shaders/shaders.pack. Reuse round6 navigation/capture scripts in out/android-pipeline-key-round7, keeping raw evidence and exact binary hashes.

Validation so far: empty bulk serializer failed byte-identity test, then host and Android regression passed. Rebuilt host integration: native_pipeline_key, guest_graphics, guest_render_state, guest_blend_request all passed. Android 500,000-call microbenchmark: legacy 339.737 / 312.595 ms, bulk 11.513 / 11.535 ms; not an FPS result. Android binary and shader hashes in out/android-pipeline-key-round7/build-identity.json.

Real-draw dual verification: 1,400,000+ comparisons including a race with 456 draws>600 frames, no mismatch/RUNTIME_STOP/HANG_REPORT. The verification run is excluded from performance results.

Four timed races completed: old combined 14.6746 FPS, bulk 15.4799 FPS over 400 intervals each (~5.5% in this sample); draw_ms 10.4369 vs 9.3301. Both bulk runs were faster than both legacy runs in corresponding windows, but no deterministic replay or SoC frequency control; not a universal gain guarantee. All mode flags and frame continuity verified, battery 36 C throughout, no runtime stops.

Delivery verified: same normal APK, original debug.env restored byte-exact, default bulk=1 / verify=0, warm cache loaded, title screen captured. docs/performance.md contains full outcome and limits. No commit requested or created.
