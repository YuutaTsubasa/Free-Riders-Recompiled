# Recorded pipeline recipes

These versioned manifests contain pipeline state and shader identities only.
They are portable input to startup preparation, not a GPU driver cache.
The release packagers include these lists unless a pack-adjacent manifest
overrides them. See [pipeline preparation](../../docs/pipeline-preparation.md).

Initial capture: 2026-10-01, shader ABI 9, release 0.4.2 shader pack
(`10b17a4d7247914fa0af8a167948d6c5709a5da77d756f1f5d34c92c949fab71`).
Both backend lists were recorded through the menus, loading and a single-player
race using the maintainer's local game installation. They are an initial coverage
sample, not complete coverage of every course, character, item or multiplayer
mode. Missing combinations continue to render normally and are learned locally.
The Vulkan list also includes a full Intro playback and the following title
screen. The initial packaged counts are 283 Vulkan and 280 D3D12 recipes.

The Vulkan list was expanded to 331 recipes using the 2026-10-01 Ally X
capture, retaining all 283 initial recipes. The capture contained 48 previously
unseen combinations, including seven shader pairs absent from the initial
route. Large hitches spent 2,880 ms and 735 ms creating pipelines; action names
were not recorded, so these cannot be definitively assigned to the reported
start boost and S-rank landing effects. The expanded list passed the runtime's
format, shader-identity and state validation and prepared all 331 entries with
none skipped on a local Vulkan device. Target-device replay is still required
to verify the reported hitches are removed.

The follow-up Ally X capture expanded Vulkan coverage to 336 recipes. It
prepared all previous 331 entries in 87.75 ms, encountered five new combinations,
and had a maximum frame time of 216.66 ms (190.20 ms pipeline creation), versus
2,939.57 ms in the previous capture. Its final 131 seconds had no frames over
50 ms (maximum 30.67 ms); the player reported clear improvement with occasional
small hitches. Settings and runtime binary matched, but play duration and
actions were not controlled, so this is not an average-FPS comparison. All 336
recipes subsequently passed local Vulkan preparation with none skipped.

The manifests now target shader ABI 10, which uses Vulkan buffer addresses
without requiring 64-bit shader integers. Their ABI field and checksum were
updated; shader identities, pipeline state records and coverage are unchanged
(336 Vulkan and 280 D3D12 recipes). The record payloads were compared byte for
byte with the ABI 9 manifests and validated with the runtime decoder. This
migration does not add coverage or imply new target-device performance results.

To update a list, use the matching backend and release shader pack, play the
scenes to include, exit normally, then copy `pipeline-cache/<backend>.manifest`
to `pipelines-<backend>.manifest` here. Validate a clean application-cache start
and a repeat start, inspect the screen output and preparation/reuse logs, and
record the capture scope. Never distribute a device's driver binary cache.
