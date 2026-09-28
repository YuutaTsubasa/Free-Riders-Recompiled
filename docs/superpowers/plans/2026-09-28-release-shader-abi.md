# Release shader ABI repair

**Goal:** Correct the incompatible Vulkan shaders shipped in v0.2.0, reported in #16/#17, and reject mixed shader/executable releases.

**Design:** Keep the renderer's v8 ABI. Rebuild shaders from v8 cache entries. Introduce pack format SFRSHPK2 with an explicit shader ABI before the count; check it in the runtime and desktop/Android packaging. Preserve existing entries and rendering behavior. Test pack production, rejection and the actual packaged game. An unversioned v1 pack is rejected with a complete-release reinstall message because it cannot distinguish v6 from v8 shaders.

**Evidence:** The released Windows pack has 468 Vulkan shaders with three push-constant fields (offsets 0/8/16). The executable sends five addresses (0/8/16/24/32). A clean v0.2.0 Vulkan run on RTX 4090 loses its device after frame 1800. Keeping the same executable and changing only the pack to the five-field shaders completes 6600 frames, including the tutorial race, with correct aspect and brightness. This does not establish the cause of #16's unlogged Grand Prix crash or D3D12 slowdown.

- [x] Reproduce the packaged-game failure; inspect compiled SPIR-V; change only the pack and compare.
- [x] Show release-packaging regression tests fail for legacy, wrong-ABI and truncated packs.
- [x] Add shared Python format validation and native header validation; update producer and packaging paths.
- [ ] Build and run native/Python tests, verify old packs stop before GPU shader use.
- [ ] Exercise a clean corrected Windows package through a race on Vulkan and D3D12; inspect screenshots and frame timing.
- [ ] Review and provide a local test build with D3D12 Agility SDK and camera dependencies. Keep issues open until affected users confirm remaining symptoms.

Scope notes: v0.1.5's origin-zero viewport regression was separately corrected in v0.2.0. The window-size setting does not change the fixed 1280x720 internal rendering resolution. Do not claim these reports prove inadequate hardware or disable 2P as a workaround.
