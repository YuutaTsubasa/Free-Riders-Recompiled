# Avatar viewport, HUD ordering and held items

User scope: correct Loading placement, keep results HUD in front of the model,
share one Avatar model setting between local players, and show held items.

## Evidence and design

- The old present-time overlay discarded the original viewport/scissor and
  drew after HUD. Capture the native character transforms and draw during the
  scene, restoring cached guest bindings after the custom pass. Runtime
  verification must establish the correct scene ordering and depth behavior;
  a first immediate-draw experiment was overwritten by later title rendering.
- `8228B310` assigns local descriptors to the list prefix; `82289C20` creates
  a partially populated local Loading list. Match a renderer/animation to that
  prefix and character 17. Do not use active controller type (+108), changed
  during results. Avatar is exempt from `8228B218` character deduplication, so
  keep two independent current-pose caches even with one model setting.
- Native held props call `8227FD58`, mapping generic hands 11/12 to Avatar
  bones 33/36, then call `823BC330` and multiply by racer+144. Empty SDK assets
  leave the native palette at identity. Replace only these hand getter calls
  with the current VRM joint transform; keep native prop geometry and offsets.
- Mesh and hand evaluation share the same hierarchy, root displacement,
  facing conversion, reflection, scale and fixed ground. Neutralize authored
  bind axes for the grip; exact Xbox prop grip calibration needs visual review.
- Keep separate upload buffers for every draw in a rendered frame and recycle
  only after GPU completion, including split-screen views of the same rider.

## Verification

- Shared local identity and CPU skin/hand regressions.
- GPU viewport/scissor, later overlay and multiple-pose upload regression.
- Scripted Vulkan/D3D12 Loading and race screenshots; inspect held items and
  results when reachable. Do not claim these based on compilation alone.
- Independent review and local test package update, preserving settings/save.

## Outcome

- The immediate pass was valid, but the old private depth let later scenery
  overwrite it. The model now writes scene depth, selecting LESS for normal
  and GREATER for inverted viewport depth. Later HUD retains title ordering.
- All 110 native tests passed. The new GPU regression also passed on Vulkan;
  it checks shared-depth occlusion in both depth directions, partial viewport
  and scissor, later overlay ordering and two poses before one submission.
- Vulkan and D3D12 completed 15,500 presents, ending only at the requested
  limit. Inspected Loading and race screenshots on both. Vulkan frame 14000
  visibly holds a can before use; matching hand traces show current pose and
  zero racer render offset. Not every prop/model grip has been checked.
- Independent review found no concrete issue in bindings, GPU upload lifetime,
  identity/capture lifetime or the scoped attachment getter.
- Actual results-screen and player-2 gameplay remain manual checks. Identity
  tests cover both locals, duplicate Avatar identity and partial Loading lists;
  GPU tests cover independent multi-draw poses. These are not an end-to-end
  two-player or results-screen gameplay claim.
