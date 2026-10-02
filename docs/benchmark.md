# Benchmarking a race without anyone playing

`scripts/benchmark.ps1` starts the game, says the menu words that reach a Free
Race (`SFR_SAY`, docs/race-controls.md), lets the race run with nobody at the
controls and stops itself. Each setting in `-Configs` runs `-Repeats` times,
**taking turns** (round 1: every setting, round 2: every setting, ...), and the
race frames of every run are summed up by `scripts/benchmark_summary.py` into
`summary.md`. Windows, PowerShell; the summaries need Python.

```powershell
scripts\build_tools.ps1 -Diagnostic
scripts\benchmark.ps1 -Configs baseline,no-suspend-notify -Repeats 6
```

The default is D3D12, uncapped (no frame limit or vsync), 720p, audio enabled,
normal elapsed race/UI time, complete rendering, and no physical player input.
Use `-Backend vulkan`, `-Capped`, or the explicit diagnostic `-NoAudio` as needed.
Every run copies a snapshot of the source save; `-SaveDirectory` selects it.
All SFR environment variables are isolated and restored afterward; only explicit
shader-pack/DXC paths are inherited. Each run records executable SHA-256 and its
effective settings in JSON. Existing output folders are rejected.
Save copies and caches stay under the new output directory; cold runs each get
a new cache, with no deletion or reuse of the player's ordinary cache.
The first run of a benchmark is a warm-up that is not counted.

## What is measured

- The frames with `racing=1` on the present line (the title's race flag), less the first `-Skip` (600) of them, which hold the countdown and the shaders and pipelines made on first use. A frame's time is the gap between its present and the one before.
- Per setting and run: mean fps, median, P95/P99 frame time, the main thread's permit hold and queueing, draw and present time, frames over 50 ms, and pipelines built during the race.
- A run that never reaches the race, or ends without a stop line, is listed and left out. The menu words are paced by the wall clock as well as by presents (`SFR_SAY_MIN_SECONDS`, `SFR_SAY_REFERENCE_FPS`), because on a fast PC a script by presents alone speaks before the menus have loaded.
- `SFR_PRESENT_LIMIT_AFTER_SAY=N` ends a run N presents after the last word.

## Reading the result

The summary does the comparison the way it should be done:

- **Per round.** Machine state drifts between rounds (heat, background work), so a setting is compared with the baseline of its own round: the table lists each round's ratio, the median, and how many rounds the setting was faster.
- **Against measured variation.** The baseline spread is reported. The automatic heuristic requires at least four pairs, 80% directional agreement, and an effect above the greater of 5% or half the baseline spread. This is not a significance test; inspect workload and scene differences before accepting it.
- Outliers show up: a baseline run with a long P99 or a large present time lifts the ratios of its round. Look at that round before believing a ratio.

On three PCs the baseline's spread was 4-8%. Treat 3% or less as a suggestion.

## Settings

`-Configs` takes names from the table in `scripts/benchmark.ps1`; each changes
one environment variable of the baseline. Among them: `no-suspend-notify`,
`no-prewarm` (with `-ColdPipelines`), `skip-draws` (the game without its
drawing, the ceiling), `serial`, `vulkan`, `constant-reuse`, `priority`,
`no-host-timing`, and `profile` (samples the main thread each millisecond
during the race; `scripts/profile_summary.py` names the functions).
Omitted-draw configurations require `-AllowDiagnosticRendering` and are never
the default comparison; their results are diagnostic ceilings, not gameplay gains.

**Two builds.** Copy each build's `out\build\host\sfr_cpu_diagnostic.exe` to
`sfr_cpu_diagnostic_a.exe` and `sfr_cpu_diagnostic_b.exe` in the same folder and
run `-Configs exe-a,exe-b`: the two alternate in the same rounds, and `info.txt`
records each file's size, build time and SHA-256. Identical executables under two
different names are rejected; `exe-b,exe-b-again` explicitly measures a control.
Settings rotate their order each round; `-FixedOrder` is available for comparison.

## On a PC that has no checkout

```powershell
scripts\make_benchmark_kit.ps1 -IncludeGame      # a folder (and .zip) with the program, scripts and your game files
```

On the other PC, unzip and run `run_benchmark.bat` (`speed`, `exe`, `pipelines`,
`author`; an optional second argument is the number of rounds). The kit holds
your own copy of the game: do not give it to anyone. `-UpdateOnly` refreshes an
existing kit in place.

## Sharing results

`scripts/anonymize_benchmark.py <run folder>` writes `<run folder>-shareable`
and a zip of it: the user name in paths, the folder the generated code was
found in, language and country, the save profile id, the computer name and
screenshots are removed; the commit, CPU and GPU names, Windows version and every
timing are kept. No file it makes is over 29 MB: a bigger result is split into
`-part1of3.zip`, `-part2of3.zip`, ... of whole files. Logs still name game
files and guest addresses, so share the summary tables rather than the logs;
`scripts/benchmark_report.py` makes a report of the tables and notes that is
safe to hand over.
The export includes only top-level evidence text (and explicitly requested BMP
screenshots); save fixtures, caches and binaries are excluded. Existing report,
shareable folder or archive outputs are preserved by refusing replacement.
Inspect exported content before sharing; anonymization cannot promise that all
potentially identifying information has been removed.

## Limits

One scenario (a Free Race, one course and character) with nobody at the controls, uncapped, on the graphics backend the PC runs. A fixed per-frame cost is a larger share of a 10 ms frame than of a 30 ms one, and a result on a desktop says nothing about a handheld.

`racing=1` means the race pointer exists, not that active driving is guaranteed.
Validate screenshots, scene boundaries and draw workload. Normal elapsed time
also means equal rendered frame counts can cover different race durations.
