# Internal rendering resolution

In the launcher's **Graphics** settings, choose **Rendering resolution** before
starting the game. Both resolution choices are together in the **Resolution** section. This controls the number of pixels drawn by the game and is
separate from **Window resolution**. Restart the game to apply a change.

| Setting | Internal pixels | Relative pixel count |
| --- | --- | --- |
| 360p / 50% | 640 x 360 | 25% |
| 540p / 75% | 960 x 540 | 56.25% |
| 720p / 100% (native, default) | 1280 x 720 | 100% |
| 1080p / 150% | 1920 x 1080 | 225% |
| 1440p / 200% | 2560 x 1440 | 400% |

Lower resolutions reduce pixel shading and framebuffer memory, which may help
GPU-limited devices. They do not remove game-logic or CPU submission costs. A
lower setting can therefore make the image softer without improving FPS on a
CPU-limited machine. Higher settings improve rendered geometry detail but cannot
add detail to the original texture assets. The output is scaled to the selected
window size with the existing aspect-ratio handling.

This is a fixed setting for the entire frame, including both players in split
screen and the HUD. It does not give each player a separate full-resolution
framebuffer or change the game simulation's 1/60-second step.

`settings.ini` stores `render_scale=100`; the runtime equivalent is
`SFR_RENDER_SCALE=100`. Supported percentages are 50, 75, 100, 150 and 200.
Missing or invalid settings use native resolution. The startup log prints
`NATIVE_RENDER_SCALE` with the actual logical and physical dimensions.

Keep the executable and `shaders.pack` from the same build together. Scaling
requires updated translated-shader helpers for resolved framebuffer textures;
the runtime rejects an older shader ABI instead of using incompatible bytecode.
The guest still sees its original coordinate space, while native color/depth
attachments, viewports, scissors and resolved framebuffer copies use the chosen
pixel dimensions.

For comparisons, keep output size, internal resolution, graphics backend and
scene fixed. Change one setting at a time and use the
[performance measurement procedure](benchmarking.md).
