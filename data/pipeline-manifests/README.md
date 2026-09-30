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

To update a list, use the matching backend and release shader pack, play the
scenes to include, exit normally, then copy `pipeline-cache/<backend>.manifest`
to `pipelines-<backend>.manifest` here. Validate a clean application-cache start
and a repeat start, inspect the screen output and preparation/reuse logs, and
record the capture scope. Never distribute a device's driver binary cache.
