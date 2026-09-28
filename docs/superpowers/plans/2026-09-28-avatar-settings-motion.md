# 1P VRM settings, materials and motion

> Execute independent launcher work using subagent-driven-development; keep renderer/rig changes together. Review before updating the local test package.

**Goal:** Address the user's four follow-ups: choose a VRM in Settings, honor double-sided materials, fix dark rendering, and add moving bones.

**Design:** Keep the existing single-player identity/world transform. Store an optional model path in launcher settings; Advanced exposes Browse/Clear and a startup hint. Per-material doubleSided and KHR_materials_unlit are respected (MToon uses its declared unlit fallback until full MToon is available). Preserve rig data to update pose, rather than reloading the model. Motion source preference was asked asynchronously; continue independent settings/material work while waiting. Do not call procedural animation original game animation.

**Evidence:** The actual knight_new_vrm1.vrm path is now available in the local test package's model-path.txt. Its materials declare doubleSided=true, KHR_materials_unlit and VRMC_materials_mtoon. Existing shader always applies Lambert with ambient .45; loader drops material flags and bakes a static pose. No embedded glTF animations.

## Launcher
- [x] Add optional `avatar_model` path to parse/format/environment. Empty path clears SFR_AVATAR_MODEL and disables the empty-assets backend; selected path enables it. Preserve Unicode and existing defaults. Test round trip, paths with spaces and clearing stale environment.
- [x] Add localized Advanced model Browse/Clear UI with selected name/path; extend existing picker with a model-file filter without changing ISO picking. Invalid path is visible; settings take effect next launch.
- [x] Build launcher and run launcher_settings test. Inspect diff for cross-platform signatures and unchanged existing input settings.

## Material correctness
- [x] Add per-primitive `double_sided` and `unlit` flags with fixtures for true/false/default, including MToon plus unlit fallback.
- [x] Create single/double-sided pipelines; select by primitive. Reverse the back-face normal for lit double-sided materials. Unlit bypasses the directional light, preserving texture colour; do not brighten the whole game.
- [x] Build shaders on Vulkan/D3D12 and compare actual model race screenshots to baseline.

## Motion
- [x] Determine whether the original Avatar animation matrices contain useful poses in the empty-assets path; otherwise implement explicitly described gameplay-driven motion as an interim option, not false original-animation support. Honor the user's source choice when received.
- [x] Preserve hierarchy, bind transforms, skin weights and humanoid names; use regression fixtures that move a weighted child joint while preserving its parent and rest pose.
- [x] Update the model safely while frames are in flight, reset pose on race transitions, and verify the actual model in motion without drifting the feet or harming 1P input.

## Delivery
- [x] Rebuild relevant/full tests, review and address findings.
- [x] Update docs and the existing local test package; migrate the selected model path into settings without losing user settings/save. Keep a recoverable prior executable. No merge/release.

## Results

- Confirmed live native animation in the raw 72-bone buffer. Retarget 19 verified humanoid joints; do not reuse the empty-assets final matrices, which remain identity. The optional source preference received no answer; proceeded with the stated game-animation default.
- User asked whether selection should happen when selecting Avatar or in Settings. Kept model selection in launcher Advanced, remembered across launches; game selection remains AVATAR. Added that instruction directly to the setting hint.
- Material flags, unlit fallback and colour-space correction verified with the selected local model. Both Vulkan and D3D12 completed 16,000-present Avatar races with expected `STOP present-limit`; inspected race screenshots.
- Full build, 110 native tests, and 238 Python tests (11 skipped) passed. Python fixtures required normal filesystem permissions after a sandbox-only failure. Final diagnostic bounds guard rebuilt and all four affected targets passed again.
- Independent review found inconsistent relative-path resolution and sheared bone bases under nonuniform ancestor scale. Both reproduced by regression tests, fixed, and re-reviewed without remaining findings. Pose tests also passed AddressSanitizer.
- The selected model has 164,245 vertices. Standalone optimized CPU posing measured about 4.6 ms/update on this machine; high-detail models remain a performance consideration.
- Updated `out/avatar-vrm-play` with matching build hashes, retained all unrelated settings/save, migrated only the model path, and backed up the previous executables/settings in `previous-20260928-204817`.
- Remaining limitations are documented: major joints only, no full MToon/blending/spring bones/face/fingers, separate model depth rather than scene occlusion, and no 2P VRM. The prior render-every-2 loading failure remains uninvestigated and the package keeps frame skipping disabled.
