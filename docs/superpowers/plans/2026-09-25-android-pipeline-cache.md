# Android Vulkan pipeline cache implementation plan

Continue the authorized performance investigation in the existing workspace, preserving all earlier changes. Execute inline with the existing systematic-debugging, writing-plans, and test-driven-development workflow.

**Goal:** reduce repeated pipeline compilation stalls, and measure cold versus warm application caches using the same APK and race setup. This does not promise an improvement to steady CPU-bound frame rate.

**Design:** Plume owns an optional device-wide VkPipelineCache used by graphics and compute pipeline creation. NativeGraphics initializes it before creating pipelines; an application-owned worker periodically exports dirty data and saves it beside the runtime data. Stop/join the worker before destroying the Vulkan device. Use Vulkan's default internal synchronization, bounded retries for a growing cache, a bounded file with integrity checking, and Vulkan vendor/device/UUID compatibility checks. Write beside the existing file and atomically replace it. Failure falls back to a fresh or disabled cache. SFR_PIPELINE_CACHE=0 preserves the original behavior; SFR_PIPELINE_CACHE_PATH selects an isolated test file.

Alternatives: keeping an in-memory cache alone cannot remove compilation across restarts; storing every application's pipeline description for prewarming is larger work and still needs a driver cache. Start with a persistent driver cache and measure it.

- [x] Add failing standalone tests for byte-exact persistence, replacement, truncation, corruption, size limits, incompatible Vulkan headers, and failed replacement preserving the destination.
- [x] Implement bounded cache file handling in src/pipeline_cache_file.{h,cpp} and run those tests.
- [x] Add the small Plume cache hook as a reproducible dependency patch; add NativePipelineCache lifecycle/background persistence and NativeGraphics integration.
- [x] Build desktop and Android graphics/runtime targets; verify dependency patches, cache file tests, and available graphics tests.
- [x] Install one APK; run the same race with disabled, cold, and warm application cache as practical. Preserve logs, cache sizes, pipeline creation times, frame continuity, draw counts, and temperatures.
- [x] Restore normal settings, leave a reviewable build installed, and document the actual outcome and measurement limitations.

Reference: https://docs.vulkan.org/refpages/latest/refpages/source/vkCreatePipelineCache.html and https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html (default internal synchronization and size-query/VK_INCOMPLETE behavior).

Verification so far: the stub failed at the first save; implemented file tests pass on Windows and arm64 Android. Desktop native graphics GPU-copy tests pass for D3D12 and Vulkan. Dependency bootstrap verifies the two Plume patches in order. Same-APK device comparison is in progress under out/android-pipeline-round5.

Device evidence: A1/B1/B2/A2 used one APK; second-window FPS 11.43 / 13.71 / 14.23 / 11.38. Pipeline creation totals 2022.84 / 616.46 / 9.41 / 2077.92 ms. B2/A2 both had 38 C battery temperature; all selected intervals consecutive, draw medians 796 / 793, but not a deterministic replay. Detailed results are in docs/performance.md.

Delivery: verified the installed APK with normal movie settings loads the default cache (2,631,092 bytes) and reaches title. Original debug.env restored; no benchmark flags remain. No commit made.
