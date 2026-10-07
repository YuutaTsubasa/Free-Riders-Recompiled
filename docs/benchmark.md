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
The run's copy (`save-<run>`, 23 MB) is removed when it ends, `fixture\` keeping
what every run started from; `-KeepSaves` keeps them, to see what the game wrote.
It must be a save that has been past the title once: a new player is asked
whether Omochao should teach them, a dialog only the Kinect hand answers, and
the menu words then go unheard, and the benchmark stops after a warm-up that
never reached a race. A kit carries one: `make_benchmark_kit.ps1 -SaveDirectory
<a played save>` copies it into the kit's `out\build\host\save` (with
`-UpdateOnly` too), which `run_benchmark.bat` then starts every run from. A save
holds your profile: keep such a kit on your own PCs.
All SFR environment variables are isolated and restored afterward; only explicit
shader-pack/DXC paths are inherited. Each run records executable SHA-256 and its
effective settings in JSON. Existing output folders are rejected.
Save copies and caches stay under the new output directory; cold runs each get
a new cache, with no deletion or reuse of the player's ordinary cache.
The first run of a benchmark is a warm-up that is not counted.

## The whole report in one go

`run_benchmark.bat full` (`scripts/benchmark_all.ps1`) runs what a performance
report needs, one after the other and unattended: what each part costs and what
limits the PC (a Time Attack stepped 1/60 s a frame: baseline, skip-draws,
render-50, no-audio, 4 rounds), then the frame rate a player gets (a Free
Race, 3 rounds). The first warm-up checks that the menus reach the race; when they
do not, nothing else runs. It ends with `out\bench\<time>-full\report.md`, both
summaries in one file to paste whole, and `out\bench\<time>-full-shareable.zip`,
the one file to attach. About 45 minutes on an i5-3470.

## Two ways to run: does a change help, and how fast is the game

| Question | Run | Why |
| --- | --- | --- |
| Does a change help? | `-Scenario solo -FixedStep` (`run_benchmark.bat exe 6 solo`) | No rivals and items, and the same race frames every run: a small difference is not lost under a heavier run |
| How fast is the game on this PC? | the default (`-Scenario race`, no `-FixedStep`) | A Free Race with rivals and items, the race stepped by elapsed time: what a player gets |

- `-Scenario solo` turns the main menu ring right to **Time Attack** (docs/race-controls.md: World Grand Prix, Time Attack, Tutorial, ...) and takes course, character and gear with `ok` (docs/pad-menus.md walked it to the character page with the pad); there is no rules page. Time Attack has no rival racers. Whether its course has item boxes was not checked. `-Say` given explicitly overrides the words of either scenario.
- `-FixedStep` sets `SFR_REALTIME_RACE=0`: the race clock (`race_frame_clock_hooks.cpp`) keeps the title's own step of one original frame (1/60 s) per present, as the desktop launcher does by default. Every run then draws the same race frames whatever the PC's speed; under 60 fps the race runs slower than the clock, which a benchmark does not mind. The summary's race time is then a race frame's number over 60, so the common stretch below is the same frames in every run.
- Every run gets a fresh copy of the save, so a Time Attack record (or ghost) of one run never reaches the next.

## What is measured

- The frames with `racing=1` on the present line (the title's race flag), less the first `-Skip` (600) of them, which hold the countdown and the shaders and pipelines made on first use. A frame's time is the gap between its present and the one before.
- Per setting and run: mean fps, median, P95/P99 frame time, the main thread's permit hold and queueing, draw and present time, frames over 50 ms, and pipelines built during the race.
- A run that never reaches the race, or ends without a stop line, is listed and left out. The menu words are paced by the wall clock as well as by presents (`SFR_SAY_MIN_SECONDS`, `SFR_SAY_REFERENCE_FPS`), because on a fast PC a script by presents alone speaks before the menus have loaded.
- `SFR_PRESENT_LIMIT_RACING=N` ends a run at its Nth present with `racing=1`: only the race is counted, not the menus and the loading screen. No limit counts from the start: a slow load presents its loading screen over a thousand times a second and could use up a fixed limit before the race. The backstop is the wall clock, 10 minutes per run unless `-TimeoutMinutes` says otherwise.
- How many: `-RaceFrames` when given. With `-FixedStep`, `-RaceSeconds` (90) × 60 = 5400, the same on every PC. Racing by elapsed time, the warm-up runs 5400 and the counted runs get as many presents as `-RaceSeconds` of the warm-up's race (`race_frames_from_warmup` in `info.txt`): 2430 at 27 fps, 27000 at 300 fps.
- A build older than `SFR_PRESENT_LIMIT_RACING` (any program of the comparison: `exe-a`, `exe-b`, ...), or `-AfterSay` given, keeps the older limit: `SFR_PRESENT_LIMIT_AFTER_SAY=N` ends a run N presents after the last word, loading screen included, so runs on different PCs race different lengths. After the warm-up the counted runs then get as many presents as `-RaceSeconds` of its race when that is more than `-AfterSay` (4200) (`after_say_from_warmup`).

## Reading the result

The summary does the comparison the way it should be done:

- **Per round.** Machine state drifts between rounds (heat, background work), so a setting is compared with the baseline of its own round: the table lists each round's ratio, the median, and how many rounds the setting was faster.
- **Against measured variation.** The baseline spread is reported. The automatic heuristic requires at least four pairs, 80% directional agreement, and an effect above the greater of 5% or half the baseline spread. This is not a significance test; inspect workload and scene differences before accepting it.
- Outliers show up: a baseline run with a long P99 or a large present time lifts the ratios of its round. Look at that round before believing a ratio.

On three PCs the baseline's spread was 4-8%. Treat 3% or less as a suggestion.

**The same stretch of race.** Every run has as many race frames, so a faster
setting covers a shorter, lighter part of the race (the countdown and the first
straight draw far less than what follows): on an i5-3470 the frame-count table
said +43% where the same seconds of the race said +18%. A second comparison
("The same stretch of race") therefore counts only seconds 20 to 75 after the race began (`--window-start`,
`--window-end` of `benchmark_summary.py`; it ends earlier when the shortest run's
race does). A fast PC plays the race presents in a short race (35 seconds at
120 fps), so when fewer than 15 seconds would be left the stretch starts at a
third of the shortest race instead. Judge by that table. With `-FixedStep`
the seconds are race seconds (frames 1200 to 4500), the same frames in every run.

`summary.md` says how it was measured ("How it was measured", after "Hardware and drivers"): the scenario,
whether the race was stepped 1/60 s a frame, and where each run ended.

**Where the frame goes** ("Where the main thread's frame goes"). For each setting the main thread's mean frame is split
into parts that do not overlap and add up to the frame: waiting for the GPU,
the rest of the present, the frame cap, other waits, queueing for a lock, CPU
draw work, and what is left, the main thread running the game's code and the HLE.
The GPU wait is inside both the present and the main thread's waits, and the
frame cap is one of those waits, so each is taken out once. A draw waits for
the GPU too when the command ring fills mid-frame (a ring flush), and that wait
is inside the draw time, so what the GPU wait exceeds the present by comes out
of the draw. The main thread's waits are counted since this change also when
it runs without the permit (the default since v0.5.0); an older build leaves
them at 0, and the summary says so.

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

## Reporting performance

`run_benchmark.bat full` gives all of the below at once. On its own,
`run_benchmark.bat report` (or `-Configs baseline,skip-draws,render-50
-AllowDiagnosticRendering -Repeats 4`) answers what limits a PC, which is what a
performance report needs. Absolute frame rates of different PCs cannot be
compared, but these ratios can:

| Setting | What it removes | If it is much faster |
| --- | --- | --- |
| `skip-draws` | every draw (a diagnostic ceiling) | the drawing costs the frame |
| `render-50` | three quarters of the pixels (`SFR_RENDER_SCALE=50`) | the GPU limits the frame |

The summary's last section ("What limits this PC") names the limit: the **GPU** when half resolution is
at least 10% faster; else the **drawing path** (CPU-side draw work and
submission) when skipping draws is at least 15% faster; else the **game's own CPU
time**, the main thread running the game's code and the HLE, which only a faster
core improves. Each ratio is the median of the setting against the baseline of
its own round, as the comparison tables are, so a laptop that warms up over
the rounds does not tilt it; the section says so when the deciding gap is not
clearly above the baseline's own run-to-run spread, and when the baseline got
slower round by round (heat). It also says how far the baseline is from 60 fps
(16.7 ms a frame), or from 120 fps on a PC already past 60. The thresholds are
a first reading, not a test; the frame breakdown above it shows why. Run it
uncapped (the default): with `-Capped` every setting can sit at the cap, and
the summary names no limit. A handheld or phone is measured with the Android
diagnostic ZIPs at two rendering resolutions instead
(docs/android-phone-diagnostics.md).

**What each part costs**. `run_benchmark.bat parts 4 solo`
(`-Configs baseline,skip-draws,render-50,no-audio,main-unpinned -AllowDiagnosticRendering
-Scenario solo -FixedStep`) changes one thing a setting and gives each one's
difference from the baseline of its round in ms a frame: no drawing at all (the
most that making drawing free could give), a quarter of the pixels (the GPU's
part), no sound sent to the output (`SFR_AUDIO=0`; the game still mixes it) and
the main thread left to Windows instead of pinned to the first core
(`SFR_MAIN_AFFINITY=0`), which reads the other way round, as what the pin saves.
With fewer than six logical processors the main thread is left unpinned by default,
so there `main-pinned` (`SFR_MAIN_AFFINITY=1`) is the one that differs from the
baseline: it reads as what leaving it unpinned saves.
`serial`, `render-every-2`, `no-vertex-cache` and `no-gpu-pipeline` are listed too
when they were run; those that turn an optimization off read the other way round,
as what it saves. `serial` is not in the mode, to keep it short. A setting whose first run never reaches the
race has its other runs skipped. A difference under half the
baseline's spread (at least 2%) is called noise. Taking a part away measures its
cost on this PC directly, where a profile only says where time was spent; this is
what tells the author what to change first. Five settings for 4 rounds take
about 45 minutes on an i5-3470.

**The slowest stretches of the race**, with `-FixedStep` only: race frame N is
then the same moment of the race in every run and on every PC. The baseline's race
is cut into half-second stretches (30 frames) and the summary lists those heavier
than the run's mean by 25% in most runs, by race frame, with their draws a frame:
the author can play the same save to that frame and see the scene. A slow frame
only one run had (a hitch) is not a scene and is not listed.

The shareable copy (`scripts/anonymize_benchmark.py`) leaves the linker map and `profile.md` out:
they name the generated code's functions. A profile is for the author's own PC.

**The PC itself** opens `summary.md` ("Hardware and drivers"), so the file is the whole
report. `scripts/hardware_probe.ps1` records into `info.txt` what can change
the numbers and whether the drivers are what the renderer needs: cores and
threads (a 12th to 14th generation Core's P/E split, read from the counts),
AVX/AVX2/AVX-512 (the game is built for Sandy Bridge: it needs AVX), memory
and its speed, every GPU with its driver's version and date, memory and
display, the chassis and Windows 11's power mode for mains and battery,
hardware-accelerated GPU scheduling, Game Mode, virtualization-based security
with memory integrity, the Windows build, the D3D12 runtime (Windows' own and
the Agility SDK beside the program) and the Vulkan loader and drivers. The
game's own log adds the adapter it drew with, its vendor, type, memory and
driver (the `NATIVE_GRAPHICS` line), and whether D3D12 was usable or it fell
back to Vulkan. Each probe stands alone and says `unknown` when it fails; none
needs administrator rights; nothing in it names the person, the computer, a
serial number or a path. `powershell -NoProfile -ExecutionPolicy Bypass -File
scripts\hardware_probe.ps1` prints the block by itself. What it cannot see, a
maker's own power mode (Turbo, Silent) or an external GPU's cable, the report
form asks for.

Open a *Performance report (PC)* issue: paste `summary.md` whole and attach the
shareable zip. Keep logs, game files and generated code out of it. A phone or
handheld goes to *Performance report (Android)* with its two diagnostic ZIPs.

## On a PC that has no checkout

```powershell
scripts\make_benchmark_kit.ps1 -IncludeGame      # a folder (and .zip) with the program, scripts and your game files
```

On the other PC, unzip and run `run_benchmark.bat` (`speed`, `report`, `exe`, `pipelines`,
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

Two scenarios (a Free Race and a Time Attack, one course and character each) with nobody at the controls, uncapped, on the graphics backend the PC runs. A fixed per-frame cost is a larger share of a 10 ms frame than of a 30 ms one, and a result on a desktop says nothing about a handheld.

`racing=1` means the race pointer exists, not that active driving is guaranteed.
Validate screenshots, scene boundaries and draw workload. Normal elapsed time
also means equal rendered frame counts can cover different race durations;
`-FixedStep` removes that, not the rivals and items of a Free Race.
The Time Attack menu words follow the documented ring and pad walk but were not
yet run unattended: if `-Scenario solo` never reaches the race, the warm-up's
screenshots (`warmup-*.bmp`) show the page it stopped on.
