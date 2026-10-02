# GPU postprocessing cost investigation and implementation plan

> **For agentic workers:** Use superpowers:executing-plans task-by-task. Do not spawn agents for this plan. User authorized autonomous private investigation and validated minimal fixes; no public push/release/comment.

**Goal:** Reduce verified Thor GPU cost without removing draws, changing guest time, discarding required depth, or regressing rendering.

**Architecture:** First attribute existing render passes and correlate guest target dimensions. Then test a narrowly scoped unused-depth attachment mode before considering full independent offscreen targets. A measured benefit and GPU output/state preservation are required before adoption.

**Tech Stack:** C++20, Plume Vulkan/D3D12, existing native rendering tests, Python private build/APK runners, ADB Thor.

## Evidence and choices

- Existing list timestamps showed substantial GPU spans despite low CPU fence waits. Coarse pass observer:11 sampled Dolphin race frames,270 passes;36.94ms/list per frame,35.91ms inside render passes,1.03ms outside. First scene pass8.38ms; repeated single-quad passes account for most remainder. Not an FPS comparison.
- GuestGraphics::set_viewport deliberately maps origin-zero offscreen surfaces to the full native framebuffer. Resolve destinations include110x90, while NativeRenderer::adopt_resolved_target copies full1280x720 images. This is compatibility architecture, not a change that can safely be fixed by only clamping viewport bounds.
- Every native scene framebuffer currently includes D32_FLOAT_S8_UINT with LOAD/STORE operations. A candidate that omits this attachment when depth AND stencil tests are disabled may remove needless bandwidth while retaining the original depth texture contents for later draws.
- Thor Adreno740 exposes VK_EXT_load_store_op_none. Still prefer a genuinely depthless framebuffer/compatible pipeline for unused-depth draws over applying NONE to a pass that reads depth. LOAD_OP_NONE leaves contents undefined inside that render pass; STORE_OP_NONE preserves prior memory only if no values were written. Never replace depth stores with DONT_CARE as a shortcut.
- Alternatives: real offscreen targets (larger correctness change involving coordinate domains, texture dimensions, resolve metadata, target lifetime,2P/VRM/loading); uniform-buffer constants instead of physical-buffer loads (Xenia difference, no causal cost established). Defer both pending the narrower experiment.

## Task1: finish attribution

Files: ignored out/continuation-20261002/gpu-pass-probe, gpu-pass-detail and matching scripts/cases.

- [x] Build copied presentation/Plume observer; no normal source/object/archive edits.
- [x] Sample original render-pass boundaries every120 presents, query only after existing fence waits. Validate query errors/overflow/list and pass consistency.
- [x] Coarse run normal-time720p/audio,15000present bound,independent source save; restore exact normalAPK/settings.
- [x] Finish detailed observer: capture first-draw viewport/scissor, varying-state flag, guest resolve dimensions. Verify actual pixels/race progress and restored package/settings.
- [x] Summarize dominant stages by geometry/pass/guest size, not ordinal alone. Retain source/APK/log/screenshot hashes and instrumentation limitations.

## Task2: unused-depth experiment design and failing regression

Candidate files: src/native_presentation.h/.cpp (framebuffer selection), src/native_renderer.cpp (compatible pipeline depth format and draw routing); tests/native_resolution_test.cpp (GPU preservation oracle). Keep renderer/layout changes confined to camera-debug worktree. Begin as isolated private experiment if performance feasibility is uncertain.

- [ ] Inspect exact pipeline compatibility and synchronization behavior before choosing depthless or supported read-only NONE. Do not remove a depth attachment from a pipeline requiring it.
- [ ] Add a meaningful GPU oracle: render overlapping depth-tested geometry, insert depth/stencil-disabled blended/color-only work, then draw geometry requiring the original depth/stencil contents. Compare resulting pixels to ordinary path. Include stencil-enabled/no-depth-test case, depth clears after a color-only pass, and repeated transitions.
- [ ] Cover custom/VRM drawing, clear and resolve paths: legacy record/clear must select suitable attachments; postprocess mode must not leak into the next list/frame. Test scales50/100/200 and Vulkan/D3D12 or gate candidate explicitly to supported backend with unchanged fallback.
- [ ] Add state assertion proving eligible draws actually use the intended attachment mode, so passing pixels alone cannot mean the candidate was never exercised.
- [ ] Run the new oracle against unchanged implementation to establish the missing mode, then implement the smallest interface/state change. Keep depth contents intact. No changed game math or omitted draws.

## Task3: implement and prove state transitions

- [ ] Add a second framebuffer over the same color texture with no depth attachment, if using depthless approach. Lifetime must outlive submitted lists; destruction occurs after original fence drains.
- [ ] Infer eligibility conservatively from both depth and stencil being disabled. Do not infer from depth-write disabled alone.
- [ ] Use a compatible depth-target format when creating eligible graphics pipelines; existing cache/recipe keys must distinguish all effective states. Check prewarmed recipes and driver cache behavior.
- [ ] Route draw recording to the matching framebuffer, preserve raster and descriptor bindings, and insert required dependencies when switching without a resolve. Avoid restarting a pass on every unchanged draw.
- [ ] Restore required attachment selection for depth/stencil clears, custom draw paths and subsequent frames. Explicitly test the transitions instead of assuming original ensure_open does it.
- [ ] Run focused GPU tests/validation on supported backends. No Vulkan validation errors or pixel differences in the preservation oracle.

## Task4: bounded same-build performance decision

- [ ] Build one private APK containing A/B toggle with identical shaders/assets; validate signing/API/alignment/source hashes. Never use external user save files directly.
- [ ] Run Thor A/B/A with normal elapsed time/audio/720p,15000present and host deadline; restore normalAPK/settings after each. Match HUD window and record draw workload, thermal availability and GPU pass spans.
- [ ] Require a repeatable difference beyond baseline variation before adopting. Observe image correctness and depth/stencil preservation, not merely frame rate.
- [ ] If positive, repeat relevant two-course/depth/VRM/2P smoke regressions on Windows and available handheld. If negative, retain evidence and drop candidate without altering production defaults.
- [ ] Update docs/performance.md and continuation handoff; local commit only for verified code/results. No publication.

## References

- Local: src/guest_graphics_state.cpp set_viewport; src/native_renderer.cpp create_draw_pipeline/adopt_resolved_target; tools/Plume/plume_vulkan.cpp VulkanFramebuffer and render-pass load/store.
- https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentLoadOp.html
- https://docs.vulkan.org/refpages/latest/refpages/source/VkAttachmentStoreOp.html
- Xenia pinned95a5c3ee: https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/vulkan/vulkan_command_processor.cc#L177

Detailed observation finished:11frames267passes;37.32ms overall,25.18ms in223 single-quad passes,all1280x720 viewport/scissor. Small guest resolves55x45/110x90/112x92; corresponding order is inference, not unique tags. Full viewport does not prove full pixel coverage. Both normalAPK restorations verified. Next must inspect actual depth/stencil eligibility in these expensive stages before implementation.

## Depth eligibility and oracle checkpoint (10:20 UTC)

- Completed private per-draw state observer: 11 race frames, 227 single-quad passes, all with depth test, stencil test and depth writes disabled. Their aggregate GPU span was 25.32ms/frame (69.94% of 36.20ms sampled list spans). This establishes eligibility, not the amount of time attributable to depth attachment traffic. Only the 11 host blits lack guest-state callbacks; all guest quad draws have known state.
- Normal-time Thor run stopped at 15000 presents, 350.563s; independent source save unchanged, normal APK hash restored, debug override removed. Frame14400 visually confirms lap2/HUD89.05s. No query errors or overflow. Artifacts: `gpu-pass-depth`, `android-gpu-pass-depth`, `thor-gpu-pass-depth-observed` under the continuation directory.
- Added a baseline GPU preservation oracle in `tests/native_resolution_test.cpp`: overlapping depth-tested geometry, blended color-only work carrying depth-write enabled, stencil-only draws, depth clear while preserving stencil, and three repeated cycles at50/100/200%. Both backends pass.
- Verified oracle sensitivity by temporarily injecting depth loss and stencil loss separately into the test. All four backend/fault combinations fail the intended pixel assertion; restored source and both backends pass again. `validate-depth-oracle.py` records guarded restoration and hashes. This is not a red/green cycle for a new attachment mode; no mode exists yet.
- Source audit: `setFramebuffer` ends a pass; `barriers` emits dependencies even for same-layout images. Switching modes without a resolve needs explicit color/depth dependencies. `record`, clear and custom VRM entry points must restore the proper framebuffer. Pipeline creation is shared by normal draw and preparation paths, so both must derive a compatible depth format. Existing keys include depth/stencil state; any mode selection must remain consistent for the renderer/cache lifetime.
- Remaining Task2 work: actual attachment-mode assertion, custom/VRM/default-record/resolve/next-frame cases, validation-layer availability. Then compatible opt-in candidate and A/B/A. No candidate implemented yet. Do not report this plan as a fix or a demonstrated performance gain.
