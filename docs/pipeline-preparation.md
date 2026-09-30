# Startup pipeline preparation

After pressing Play, a build with a pipeline manifest prepares its known graphics
pipelines before the first game frame. The game window shows a progress bar and
processed/total count. Close the window or press Escape to cancel; Android uses
a long Back press. An already running driver compilation cannot be interrupted:
cancellation stops further work and safely waits for that call to return.

The first start can take longer. The Vulkan driver cache is loaded before
preparation and saved on this device, so later starts normally prepare much
faster. A cache file alone does not guarantee every pipeline exists. Updating
the GPU driver can invalidate that cache and require compilation again.

`shaders.pack` contains translated shader code. A manifest is a separate list
of observed shader combinations, vertex layouts, and raster/blend/depth/stencil
settings. It contains no game textures, meshes, user settings, or driver binary
cache. Source identities and the shader ABI identify the stages; pointer values
are never saved. The actual pipelines are created with this device and backend.

The packaged lists live beside `shaders.pack`, as `pipelines-vulkan.manifest`
and `pipelines-d3d12.manifest`. New observed combinations are added to
`pipeline-cache/vulkan.manifest` or `pipeline-cache/d3d12.manifest`. Both lists
are deduplicated on startup. Learned files are saved periodically and on clean
renderer shutdown using atomic replacement. Keep `pipeline-cache` when moving
an existing installation to preserve its learned recipes and driver cache.

Missing optional lists permit normal startup and learning. Invalid, truncated,
wrong-backend or old-ABI lists are ignored with a diagnostic. A recipe whose
shader is absent from the current pack is skipped during preparation; it does
not invoke the development translator. Invalid shader packs retain the existing
installation error. This feature never skips a draw: combinations not prepared
still compile on first use and are recorded for later starts.

## Coverage and recording

A list collected from one route is not exhaustive. Other courses, characters,
items, two-player modes and custom avatars may still introduce work during
play. Render-target creation, texture upload and other loading work are also
outside pipeline preparation. No game clock or FPS cap is changed.

For recording, use the exact release shader pack and run representative scenes.
After a normal exit copy the corresponding learned manifest beside that pack
under its `pipelines-<backend>.manifest` name. The desktop and Android package
scripts include adjacent manifests automatically, falling back to the recorded
lists in `data/pipeline-manifests` when no adjacent list exists. Runtime validation
discards recipes that do not match the current shader pack. Verify a fresh start with an
empty driver cache and compare `PIPELINE_PREWARM` completion/hit counts against
the remaining `NATIVE_PRESENT pipelines` and `pipeline_ms` fields. Repeat with
the saved cache, checking actual screen output as well as timings.

Developer overrides:

| Variable | Purpose |
| --- | --- |
| `SFR_PIPELINE_PREWARM=0` | Disable preparation for a same-executable comparison; continue recording. |
| `SFR_PIPELINE_MANIFEST` | Override the packaged manifest path. |
| `SFR_PIPELINE_MANIFEST_LOCAL` | Override the learned manifest path, e.g. isolate a capture. |
| `SFR_PIPELINE_CACHE_PATH` | Existing Vulkan driver-cache override, independent of the manifest. |

Preparing on a fast desktop does not establish loading time or stutter reduction
on a handheld. Validate the supplied list on the target device before claiming
that its first-run or countdown hitch is fixed.
