# Exporting Android performance diagnostics

In builds that include **Export diagnostic ZIP**, use **Settings → Game files → Export diagnostic ZIP** after playing. Choose a destination such as Downloads and wait for **Diagnostic ZIP saved**. Cancelling the picker does not export anything. No USB connection or storage permission is required.

For persistent slow races, compare the same track with **Rendering resolution** set to 720p and then 360p. Keep other settings unchanged. Play for 30–60 seconds in each run, close the game, and export after each run **before starting the next game** (which replaces `game.log`). Send both ZIPs with which resolution each used and whether lower resolution felt faster. If the device gets hot, let it cool before repeating; the results otherwise mix rendering changes with thermal throttling.

The archive includes app version and device information, selected performance settings from the last launch, and the game log. It does not recursively copy app storage or include saves, game data, or camera images. A log larger than 4 MiB retains its first 256 KiB and recent tail with a truncation marker. Logs can contain local file paths; review before sharing.

Quiet Android runs emit `NATIVE_FRAME_SUMMARY` after each 120 measured guest frame intervals. `guest_fps` is guest frame submission frequency, not Android display refresh rate; `rendered` distinguishes skipped rendering. Mean/max frame time includes pacing and scene transitions. Present/GPU-wait fields measure CPU wall time in those calls, **not GPU execution timestamps**; fields can overlap and must not be added as independent frame costs. Pipeline and texture totals help identify compilation/upload stalls. Detailed `SFR_FRAME_METRICS` logging remains optional and replaces the summary.

A resolution comparison can narrow the investigation; it does not by itself prove a CPU or GPU bottleneck. CPU sampling or a GPU trace may still require USB access later.
