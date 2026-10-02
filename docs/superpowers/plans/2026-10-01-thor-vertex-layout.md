# AYN Thor vertex-layout preparation

Goal: Reduce native_draw CPU work without changing guest clocks, geometry, shaders or image quality. The latest 30-second race CPU sample attributes 3.7% of main-thread leaf CPU to native_draw, alongside its helper/PLT costs. Each draw currently parses a declaration, maps semantics/formats, and fills 20 shader input locations anew.

Design: Extract the existing declaration parser into a pure decoder of bounded big-endian element bytes. Keep a per-thread, 16-entry cache keyed by declaration identity, stride and every input byte. Always reload the declaration count and content; this detects same-address edits and remapping. Read directly only if GuestMemory.fast_read accepts the entire ordinary range; otherwise retain scalar checked loads of exactly the original fields. The decoded value owns its element list, DEC3N offsets and swap masks. The renderer still receives the same NativeDraw and data. Include an environment-only uncached/verification mode for controlled experiments.

Steps:
1. Add decoder/cache regression tests for format mappings, missing inputs, same-address edits, stride-sensitive DEC3N, count changes, eviction and invalid declarations; add the test target to build/CI lists.
2. Extract the unchanged parsing rules, implement bounded content-validated cache and integrate native_draw. Preserve diagnostics and fallback reads.
3. Run host and Android tests, a deterministic decoder benchmark, cache-versus-reference runtime verification and Windows render smoke.
4. Run AYN Thor race comparison against 80ad835 with normal timing and existing 720p settings. Match race phase with screenshot HUD clocks, retain raw evidence, and avoid calling small/unmatched FPS changes a gain.
5. Independently review, document evidence, install a normal test APK only if correctness checks pass. No release publication is requested.


Implementation and correctness evidence:
- Added `NativeVertexLayoutCache`: 16 owned entries, bounded byte snapshots, content/stride/address validation on every draw. Original scalar reads remain the fallback for ranges rejected by `GuestMemory::fast_read`.
- Review caught a key/value race if guest bytes were read twice. Fixed by comparing, decoding and storing one snapshot; follow-up review found no remaining actionable issues.
- `SFR_VERTEX_LAYOUT_CACHE=0` uses the original parser; `SFR_VERTEX_LAYOUT_VERIFY=1` compares both parsers. Normal play does not run the reference comparison.
- Host CTest: native_vertex_layout, native_formats and native_pipeline_key all pass. Android target builds. Regression coverage includes same-address mutation, stride/count changes, eviction, component swap masks and invalid declarations.
- Thor reference verification exceeded 3 million draws, including Dolphin Resort race, without mismatch. Windows D3D12 reference verification exceeded 6.1 million draws and Vulkan 5.8 million draws, both into the race without mismatch. Final snapshot-only fix was applied after the Thor reference run and before Windows/reference and Thor measurement builds.
- Final Android measurement: HUD advances 12.38 -> 38.97 seconds over screenshot timestamps 26.568 seconds apart. Game time is not slowed to make presentation smoother.
- Same-phase early-race sample: prior scheduler build 22.9606 FPS, layout candidate 23.0702 FPS. Same-APK cache-off measured 23.5883 FPS (784.43 draws/frame), and cache-on repeat 23.4310 FPS (791.42 draws/frame). This does not demonstrate a repeatable FPS gain; keep it a local test branch rather than a release claim.
- Android CPU profile captured 19,255 samples over 30.0045 s, none lost. Remaining main-thread leaf work includes TLS resolution 4.66%, PLT 4.31%, memcpy 3.27%, sub_82534038 3.02%, sub_824F56E8 2.97%, native_draw 2.88%. Percentages are CPU samples, not frame-time fractions; different race phases prevent treating leaf changes as exact savings.
- Prepared a private local-function-link experiment, but did not install/adopt it: older tests already showed an unexplained loading failure and no repeatable speedup. Production linker options remain unchanged.

Raw local evidence and reproducible runners: `out/thor-race-perf/` (ignored), including layout-verify.log, layout-measure.log/.data/-run.json, layout-v2-symbols, Windows reference logs/screenshots, phase_compare.py and run.py. Test save data is isolated under benchmark-save on the device.

Final verification: maximum declaration and failed-decode cache-preservation tests added; host three-target CTest and final Android native_vertex_layout test pass. All three measured runs stopped at their intentional present limits, without a mismatch or game crash. Normal APK SHA256: 5e0de1bc8a857db3f21bb96a1b1092eb239cca79c2bf225515fc1db497cd7925; libmain.so and shaders.pack match the measured profileable APK byte-for-byte; normal manifest has no profileable entry.
