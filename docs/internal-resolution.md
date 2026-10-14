# Internal rendering resolution

In the launcher's **Graphics** settings, choose **Render Size** before
starting the game. It sits directly below **Graphics backend**, while the window
size (shown only with Display Mode **Windowed**) is near the top of the page.
Render Size is a **resolution scale** of the output: the
window size, or the display size in full screen (Display Mode **Borderless**). Restart the game to apply a
change.

The slider starts at **Original** (the default): the game's own 720 lines
(1280 x 720 at 16:9) whatever the window or display, as every earlier version
drew, so a handheld or a phone keeps its frame rate after an update. The other
stops go from 0.25x to 3.0x in steps of 0.25x (0.25x, 0.5x, 0.75x, 1.0x,
1.25x ... 3.0x). Some of the choices:

| Resolution scale | Internal height | Example: 1920 x 1080 window | Example: 2560 x 1440 window |
| --- | --- | --- | --- |
| 0.25x | a quarter of the window's | 480 x 270 | 640 x 360 |
| 0.5x | half the window's | 960 x 540 | 1280 x 720 |
| 0.75x | three quarters | 1440 x 810 | 1920 x 1080 |
| Original (default) | 720 lines | 1280 x 720 | 1280 x 720 |
| 1.0x | the window's | 1920 x 1080 | 2560 x 1440 |
| 1.5x | 1.5 times | 2880 x 1620 | 3840 x 2160 |
| 2.0x | 2 times | 3840 x 2160 | 5120 x 2880 |
| 2.5x | 2.5 times | 4800 x 2700 | 6400 x 3600 |
| 3.0x | 3 times | 5760 x 3240 | 7680 x 4320 |

The pixel count grows with the square of the multiplier: 2.0x draws 4 times the
pixels of 1.0x and 3.0x draws 9 times, while 0.5x draws a quarter and 0.25x a
sixteenth. GPU cost and memory follow the pixel count, so the top choices are
meant for strong GPUs.

With **Aspect Ratio** 16:9 the internal frame keeps the game's 16:9 shape;
only its height follows the window (a 3440 x 1440 window at 1.0x renders
2560 x 1440). With Auto, a window wider than 16:9 widens the frame to its shape
(3440 x 1440 at 1.0x renders 3440 x 1440, and Original renders 1720 x 720).

An older `settings.ini` with `render_scale` (a percent of 1280 x 720) is read
once: 100 becomes Original, any other value the step that draws about as many
lines in the saved window.

1.0x draws exactly as many lines as the window or desktop shows, so the picture
is not upscaled. Values below 1.0x reduce pixel shading and framebuffer memory,
which may help GPU-limited devices; values above 1.0x supersample (draw more
pixels and scale them down) for smoother edges, at a greater GPU cost. Neither
removes game-logic or CPU submission costs, and higher values cannot add detail
to the original texture assets.

The window size itself defaults to the desktop size when `settings.ini` has no
`window_width`/`window_height` (a first start); once saved, the chosen size is
kept. The list's **Desktop size** entry selects it again. The list's last entry,
**Custom**, shows width and height boxes (160 to 16384 pixels each; applied when
you press Enter or leave the box, saved in the same `window_width`/`window_height`
keys). A saved size the list does not offer appears as Custom on the next start.

This is a fixed setting for the entire frame, including both players in split
screen and the HUD. It does not give each player a separate full-resolution
framebuffer or change the game simulation's 1/60-second step.

## How it reaches the game

`settings.ini` stores `resolution_scale` as a percent of the output size
(25, 50, 75, ... 300, any multiple of 25 from 25 to 300; anything else reads
as 100). The older
`render_scale` key (a percent of 1280 x 720) is ignored, so an old file loads
at 1.0x; it is not written again.

The game keeps rendering in its 1280 x 720 logical frame. The launcher converts the scale
into `SFR_RENDER_SCALE`, a whole percent of those 720 lines:

```
SFR_RENDER_SCALE = round(output_height * resolution_scale / 720)
                 = round(output_height * multiplier / 720 * 100)
```

where `output_height` is the window height, or the display height in full screen
(always on Android). For example 1440 lines at 1.0x give 200, 1080 at 1.5x give
225 and 1200 at 1.0x give 167 (1202 lines).

Started without the launcher, `SFR_RENDER_SCALE` keeps that meaning: a percent
of the native 1280 x 720 logical frame. The runtime accepts any whole number,
clamped to 25..800 (0.25x of a 720-line window asks for 25%, the minimum, so
the frame is 320 x 180; a window below 720 lines at 0.25x is raised to 25%);
text that is not a number falls back to 100. Each side of the
scaled frame is rounded to the nearest pixel, and to the nearest even pixel when
the logical side is even (the real frame's always are), so the internal size is
never odd.

Targets are limited to 8192 pixels a side. When the logical frame scaled by the
percent would exceed that, the runtime lowers the percent (both sides
together, so the aspect ratio is kept): for example 3.0x of a 2160-line display
asks for 800% (900% clamped), and a 1280-wide frame is drawn at 640%, 8192 x 4608.
The game's own render surfaces scale by the same ratio and are also capped at
8192 a side. The startup log prints `NATIVE_RENDER_SCALE` with the percent used,
the logical and physical dimensions, and `requested=` when it was lowered.

Keep the executable and `shaders.pack` from the same build together. Scaling
requires updated translated-shader helpers for resolved framebuffer textures;
the runtime rejects an older shader ABI instead of using incompatible bytecode.
The guest still sees its original coordinate space, while native color/depth
attachments, viewports, scissors and resolved framebuffer copies use the chosen
pixel dimensions.

For comparisons, keep output size, internal resolution, graphics backend and
scene fixed. Change one setting at a time and use the
[performance measurement procedure](benchmarking.md).
