# Single-player Avatar and VRM Integration Plan

> **For agentic workers:** Execute this tightly coupled port in order; use requesting-code-review before delivery. Track steps below.

**Goal:** Integrate PR #15 and the existing local VRM implementation on current main, then produce a testable 1P Avatar build.

**Architecture:** Preserve the external PR's Avatar body-type contribution, extend the Avatar import path with the local empty-assets implementation, and render a user-provided VRM only for the active 1P Avatar. Preserve current rendering, camera-input and controller code while importing the model loader, texture decoder and shaders. Existing fixed-camera placement must be investigated, not presented as a true world transform.

**Tech stack:** C++20, generated PPC guest code, Plume D3D12/Vulkan, glTF/VRM, CMake, native and Python tests.

## Agreed scope

The user explicitly chose to get 1P correct first. Work from main at v0.2.1 in the existing clean camera-debug worktree on codex/avatar-vrm. Do not alter the dirty primary checkout. Keep custom model files and saves local. Deliver a runnable test build; do not merge or publish a new release as part of this task.

## 1. Port the existing implementation

- [ ] Cherry-pick origin/pr/15, preserving attribution.
- [ ] Integrate local Avatar/VRM commits 20bb8dc, e038eea, ec17fb6, ce7c9c5, 2d496b4, 5fc60d4, 34a1e96, fe92c62, d5f081c. Resolve body-type duplication once, retaining manifest diagnostics. Import only changes belonging to Avatar/VRM; preserve latest main on unrelated code.
- [ ] Files: diagnostic_main.cpp (imports), game_patches.cpp (empty-part guards), generate_diagnostic.py/diagnostic_hooks.h/vector_compare_bounds.* (vcmpbfp), gltf_model.* and image_decode.* (model/textures), native_presentation.* and shaders/model.hlsl (rendering), guest_graphics_hooks.cpp (render placement), CMakeLists.txt and build_tools.ps1 (tests/builds).
- [ ] Run gltf_model, image_decode and vector_compare_bounds native tests and generator Python tests. Inspect generated sub_827569B8 for the implemented compare rather than a trap.

## 2. Make rendering specific to the 1P Avatar

- [ ] Trace current player/character selection and the board/rider transform using the existing race hooks and original generated code. Add regression coverage for Avatar character IDs 17/18, ordinary riders, loading/menu transitions and two-player mode.
- [ ] Draw only for the active single-player Avatar. Clear per-frame placement so a prior scene cannot leak a stale transform. Check model scale and foot placement against the board during racing; report any remaining fixed-camera limitation explicitly.
- [ ] Keep model parse failures visible in the log without terminating normal game operation. Validate the existing PNG/JPEG and glTF loaders before exposing the file path to test builds.

## 3. Verify and deliver

- [ ] Regenerate/rebuild guest code if the imported instruction support changes it; do not reuse incompatible generated objects.
- [ ] Build the Windows launcher/game and run appropriate native and Python tests.
- [ ] Use the existing local Avatar save/model when available. Verify selection -> loading -> race on Vulkan and D3D12, inspect screenshots, and check ordinary-character rendering with the VRM path set.
- [ ] Request a bounded review of the combined diff; address concrete findings.
- [ ] Prepare a local test launcher/package with clear controls, current shaders and runtime, preserving user saves/settings and excluding private model/game assets from commits.

## Known starting limitations

The old VRM experiment uses a baked riding pose, base-colour textures, an independent model depth buffer and camera-relative placement. Dynamic animation, full MToon, scene occlusion and two separate player models are not established capabilities. Verification must distinguish actual world placement from a screen-space approximation.
