# Internal rendering resolution

In the launcher's **Graphics** settings, choose **Render Size** before
starting the game. It sits directly below **Graphics backend**, while the window
size (shown only with Display Mode **Windowed**) is near the top of the page.
Render Size is a **resolution scale** of the output: the
window size, or the display size in full screen (Display Mode **Borderless**). Restart the game to apply a
change of the setting. On a computer the render size then follows the window
while you play: resize the window (or switch full screen with Alt+Enter) and
the game draws at the chosen scale of the new size, without a restart (see
[Resizing the window](#resizing-the-window)).

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

## Resizing the window

On a computer (Windows, Linux, macOS) the game window can be resized while the game
runs, by dragging its edge, maximizing it, or switching full screen with
Alt+Enter (Issue #365). What follows the new size:

- **The picture** follows at once, every frame of a drag: the swap chain is
  made again at the window's size and the frame is scaled into it.
- **The render size** follows once the size has settled: 0.2 seconds after it
  last changed, or as soon as you let go of the window's edge on Windows.
  The framebuffer and the game's own render surfaces are then made again at
  the chosen Render Size of the new window, as if the game had started at that
  size. At 1.0x a 1280 x 720 window drawn at 1280 x 720 becomes a 1920 x 1080
  window drawn at 1920 x 1080; at 0.5x it would be 960 x 540. **Original**
  keeps 720 lines whatever the window, so only the picture's scale changes.
  Making the targets again takes a few milliseconds, once, and the game goes
  on where it was (in a menu or in the middle of a race).
- **The shape of the frame** (Aspect Ratio **Auto**) is still the one the
  game started with. A window of another shape shows the frame with black bars
  at the sides or at the top and bottom, its proportions and the HUD as they
  were; restart the game to draw the new shape (for example to widen a 16:9
  start to an ultrawide window). The title reads its screen width once, when it
  creates its graphics device: the width is copied into its own graphics
  object, its 2D screen size and its cameras' default aspect, and a race lays
  out its views from it, so changing it while the game runs is not done.

Minimizing the window changes nothing: the game skips drawing while it is
minimized and comes back at the same size. The log has a line for each
settled size and each new set of targets:

```
NATIVE_RENDER_SIZE window=1920x1080 percent=150 was=100
NATIVE_RENDER_SCALE rescaled percent=150 logical=1280x720 physical=1920x1080 was=1280x720 ms=1.6
```

The launcher passes the Render Size to the game as `SFR_RENDER_SIZE` (0 for
Original, otherwise the percent of the output's height, as `resolution_scale`
in `settings.ini`). Started without it, the render size stays what
`SFR_RENDER_SCALE` made it, as before. Phones and tablets do not set it: their
frame fills the display and does not follow a window.

For tests, `SFR_TEST_WINDOW_RESIZE=<present>:<step>,...` makes the game change
its own window at the given presents, where a step is `<width>x<height>` (the
client area), `min`, `restore`, `full` or `window`.

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
