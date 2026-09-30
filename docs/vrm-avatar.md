# VRM Avatar guide

Custom Avatar models are experimental in v0.3.0. They replace the in-game
AVATAR rider, not Sonic or another ordinary character. Your save must already
have AVATAR available; the model setting does not unlock it.

## Choose a model

In the launcher's **Avatar models** tab, choose **Browse** and select
your own `.vrm` or binary `.glb` file. VRM 0.x and VRM 1.0 humanoid mappings
provide animation; an ordinary GLB without that mapping stays static.

Start the game, select **AVATAR**, and choose Gear. You can use a controller,
keyboard or the optional Windows Camera motion input. Selecting a VRM does
not enable the camera and does not require one.

There is one remembered model setting for both local players. Whoever
selects AVATAR uses it, with independent animation poses for each player.
Other characters retain their original appearance. Full two-player VRM
gameplay still needs broader testing.

Changes apply on the next game launch. Return to the launcher to change the
model; **Clear** disables it. The setting is stored as `avatar_model` in
`settings.ini`. Model loading is local and does not upload the file.

## Platform details

- **Windows:** Browse uses the file picker. The launcher remembers the path,
  so leave the model there or select it again after moving it.
- **Linux:** Browse uses `zenity` or `kdialog` when available. You can also
  type the path into the model field.
- **Android:** Browse opens the system document picker and imports a copy
  into the app-specific storage. Select a replacement to update that copy.

Desktop relative paths resolve from the launcher folder. A missing file
shows a warning. If a model cannot load, inspect `game.log` for
`NATIVE_MODEL unavailable`; select another model or clear the selection.
No personal model or game save is included in releases.

## What to expect

The model follows the game's main humanoid animation, root motion and stance
reflection. It renders with the scene viewport and depth, before subsequent
HUD drawing. Held props use its hand joints while the game retains control
of item behavior. Double-sided and unlit materials are supported.

Current limitations:

- Full MToon shading and transparent blending are not implemented.
- No spring bones, facial expressions or finger animation.
- Only the main humanoid joints receive game animation. Kinect procedural
  shoulder corrections that rely on absent Xbox Avatar bind data are omitted.
- Bone proportions and grip orientation vary by model. Some poses and item
  grips may need further adjustment; not every item has been visually tested.
- Skinning runs on the CPU. Large meshes and textures can increase load time,
  memory use and frame time. Try a simpler model if performance drops.
- Windows D3D12 and Vulkan Loading/race rendering have been tested. The actual
  results screen, full two-player play and physical Linux/Android VRM gameplay
  need more coverage.

If reporting a problem, include the release version, platform, graphics
backend, VRM version, affected screen/action and relevant `game.log` entries.
Only share a model file if its licence allows you to do so.
