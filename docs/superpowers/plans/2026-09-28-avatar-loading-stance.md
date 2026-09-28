# Avatar Loading and stance follow-up

User reported an invisible model in Loading, a twisted torso during ordinary
forward riding, and missing airborne/side-changing motion when changing stance.

## Evidence and fixes

- Loading uses the same main Avatar draw call, LR `822AB04C`, camera 0. The
  race manager's `+20` is planned count 12, while its vector initially has only
  one local preview rider. `822885F8` sets planned count; `82289C20` creates the
  local previews; `82289E00` later builds the full list. Removed only the check
  that planned count cannot exceed populated count. Kept all other lifetime,
  single-player, vector-range and character checks. Red/green regression covers
  the partial list and transition to the complete list.
- `822A5140` reads native chest matrix 5 via `823BC330` to derive shoulder
  corrections. Empty Xbox assets leave these matrices at identity. Subsequent
  `822A6988` / `823BC258` writes therefore turn absolute shoulder yaw into an
  incorrect local chest correction: about -78 degrees root plus -82 chest.
  Capture evaluated/blended animation after `823BBCB8`, before those writes.
  Match controller/renderer/buffer to current 1P and key the snapshot by
  rider/buffer/present; tests reject stale and foreign snapshots. The original
  game state is not modified. Full native procedural Kinect joint corrections
  remain unsupported without the native bind skeleton.
- Root translation reaches about +0.96 metres during stance changes. It was
  discarded, and two dynamic minimum-Y anchoring steps erased vertical motion.
  Apply root translation once, retaining fixed authored bind ground height.
  Other translations stay unused to preserve model proportions. Tests cover
  finite rejection, fixed baseline, root displacement and no drift.
- `822821B8` sets byte `[renderer+8]` from stance. `823BA028` uses that byte to
  insert a root X reflection. Reflect completed VRM geometry/root translation
  and normals, and use the reversed static triangle-index buffer. Tests cover
  reflection, VRM0 facing, normals and repeated toggling.

## Validation / delivery

- Full build and all 110 native tests passed. Independent review found no
  remaining concrete bugs in capture lifetime, translation, reflection, or GPU
  buffer lifetime.
- Vulkan scripted Loading/riding/two stance changes/jump completed 13,800
  presents. All 361 sampled clips matched the current frame; mirror changed
  0 to 1 then back to 0. Inspected Loading, cruising, airborne and opposite
  stance screenshots. Expected `STOP present-limit` only.
- D3D12 also completed 13,800 presents with expected limit stop, both stance
  mirror states, and 368/368 valid current-frame clip samples. Inspected Loading
  and airborne stance screenshots. Refreshed the existing local package with
  an executable/settings backup and unchanged model selection, input and save.
