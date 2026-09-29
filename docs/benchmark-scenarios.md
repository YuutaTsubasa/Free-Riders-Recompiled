# Reproducible feature comparisons

A standard-library Python runner stages one release package per guided
capture, opens that copy's existing launcher, and records the inputs needed to
repeat the run. It never edits the source package or save. Four scenarios use
the same common settings: `1p`, `2p`, `camera` (webcam motion), and `vrm` (one
controller player with a custom avatar). Camera, avatar, voice and player-two
settings are forced to the selected scenario. Live play remains manual: menu
navigation, body tracking, second-player participation and race alignment cannot
be proved by a launcher invocation.

## Prepare a repeatable protocol

Use a packaged Windows or Linux desktop release with its matching shader pack
and runtime libraries. Android launcher capture is not supported by this script.
Supply your own decoded image directory (`complete.txt`, `image.bin`), extracted
disc assets (`default.xex`), save seed and common `settings.ini`. No copyrighted
game data is bundled or downloaded. The runner references image/assets in place;
the game treats them as input. It copies the save seed before each run.

Write a UTF-8 protocol file recording the exact mode, course, character, gear,
route/actions, warmup, and HUD-time interval to measure. For 2P use a fixed pair
of characters and controller assignments and specify both players' actions.
Record display refresh, GPU/driver, power mode and connected input device names;
host OS and CPU are recorded automatically. Warm the course once and optionally
supply the same warmed pipeline file and shader-cache directory to every run.
An omitted cache seed means a cold cache, which must be compared separately.

| Scenario | Active feature | Required equipment and manual verification |
| --- | --- | --- |
| `1p` | One controller/keyboard player, standard rider | Same input device and route. Confirm solo mode. |
| `2p` | Two controller/keyboard players, standard riders | Two people/devices or distinct keyboard bindings; configure player 2 in the common settings and join a two-player mode. Confirm both riders are active. |
| `camera` | One player, webcam motion, standard rider | Explicit camera name, pose and detector ONNX files, compatible runtime, full-body space and fixed lighting. Confirm tracking and camera-driven movement; gamepad takeover makes the sample incomparable. |
| `vrm` | One controller/keyboard player, custom avatar | Explicit VRM/GLB path supported by the build. Confirm the model is actually visible; a load failure is not a VRM result. |

The camera scenario measures webcam motion, not a Kinect or picture-only mode.
It intentionally leaves the skeleton debug window and voice off. For camera
repeat the same poses/actions, camera position and tracked interval; this is a
guided regression protocol, not a deterministic camera replay. Compare each
scenario against its own previous captures. A 2P scene has different workload
from a 1P scene and cannot isolate a particular optimization by itself.

## Run

Run from any working directory with Python 3.10 or newer. In PowerShell:

```powershell
$common = @('--package', 'C:\Games\FreeRiders', '--settings', 'C:\Bench\settings.ini',
  '--image', 'C:\GameData\image', '--assets', 'C:\GameData\assets',
  '--save', 'C:\Bench\save-seed', '--output', 'C:\Bench\captures',
  '--protocol', 'C:\Bench\protocol.txt', '--backend', 'vulkan', '--render-scale', '100')
python scripts/benchmark_scenarios.py @common --scenario 1p --dry-run
python scripts/benchmark_scenarios.py @common --scenario 1p --validate
python scripts/benchmark_scenarios.py @common --scenario 1p
python scripts/benchmark_scenarios.py @common --scenario 2p
python scripts/benchmark_scenarios.py @common --scenario camera --camera-device 'USB Camera' --pose-model C:\Models\pose.onnx --pose-detector C:\Models\detector.onnx
python scripts/benchmark_scenarios.py @common --scenario vrm --avatar-model C:\Models\rider.vrm
```

`--dry-run` only prints the proposed configuration and does not require paths to
exist. `--validate` checks files and isolation without staging or launching;
neither mode verifies assets, models, hardware or game compatibility. Real runs
copy package runtime files into a new output directory. Use an output directory
separate from every source input, with space for one package copy per capture.
The original package's game, saves, logs, caches and settings are excluded.
Symbolic links in copied inputs are rejected to avoid escaping isolation.

The launcher opens with scenario settings applied. Follow the saved protocol,
play exactly once, then close the game and launcher. Do not change graphics or
feature settings in that launcher. Close all other game instances before
starting. A per-user system-temp lock prevents concurrent invocations of this
runner, including invocations with different output roots; it cannot control a
game launched independently. Let the runner finish before starting another run.

Use `--pipeline-cache PATH` and `--shader-cache DIRECTORY` for warmed cache
seeds. Their copies remain private to each capture. No arbitrary `SFR_*`
overrides are accepted: inherited ones are removed and recorded separately.
The launcher retains the 60 FPS cap; `race_render_every=1` prevents frame
skipping. `--render-scale` controls the requested internal scale separately
from the common settings' output window dimensions. Older executables can
ignore the setting: inspect startup resolution diagnostics/screenshots before
calling a lower-resolution capture valid.

## Inspect evidence before comparing

Each output has `run.json`, common and scenario settings snapshots, the protocol,
input/cache inventories with SHA-256 hashes, and the isolated package containing
the game's `game.log`, final settings, saves and caches. Executable hashes and
the exact controlled launcher environment are recorded. The launcher creates
the child game's settings-derived environment; metadata labels this distinction
instead of claiming to have inspected the child process environment. Runtime
backend, internal resolution, camera tracking and avatar activation still need
to be checked in logs/screenshots. Environment metadata excludes unrelated
host variables which may contain credentials.

A captured log is marked **captured-unverified**, not a passing benchmark.
No new log, no frame rows, or changed scenario settings invalidates capture.
Equivalent launcher rewrites (`false` to `0`, `true` to `1`, and leading zeros
in supported integer settings) are normalized before comparison. Unknown or
invalid values remain literal so unexpected fallback changes are detected.
Captures require the child's normal `STOP window-closed` completion record;
frames followed by a truncated log are incomplete. Nonzero launcher exits,
`Diagnostic error:` exceptions and other runtime stops also invalidate capture,
even when the launcher itself exits successfully. The full log remains available
for diagnosis.
Inspect startup diagnostics for requested backend/internal resolution and
feature errors, confirm the scene with screenshots/HUD time, and reject stops,
fallback camera input, missing avatars or incomplete frame windows. Record
verified frame boundaries separately and analyze them using
[benchmark_frames.py](../scripts/benchmark_frames.py), following
[benchmarking.md](benchmarking.md). Repeat baseline/candidate/candidate/baseline
with the same seeds and protocol, report all runs and variation, and compare
frame-time tails and non-pacing time at the fixed cap. These steps are required
before making a performance or regression claim.
