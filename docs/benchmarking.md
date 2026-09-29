# Measuring performance without speeding up the game

The original game advances approximately 1/60 second per presented frame. Keep
the launcher's normal 60 FPS cap: uncapping the game also accelerates its logic.
Rendering more frames safely would require separating simulation from rendering;
this benchmark does not make that change.

## Record a comparable run

Use the same executable, graphics backend, resolution, render-every-frame setting,
character, gear, course, player count and input source. Start with Camera, VRM and
voice off to measure the base game; test those features separately afterward.
Keep VSync and power settings the same. Close other games and CPU/GPU workloads.

Warm up the course once. Then run **legacy / fast / fast / legacy** to compare the
GPU-fence wait and Windows thread-placement optimizations in the same build. Use a solo/time-trial course when
possible, repeat the same route and actions, and measure the same part of the
race. Separate loading/first-time shader compilation from steady play. Copy the
same warmed pipeline cache into each isolated run when automating this.

On Windows, from the repository:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/capture_benchmark.ps1 -PackageDirectory C:\Games\FreeRiders -Mode legacy
powershell -ExecutionPolicy Bypass -File scripts/capture_benchmark.ps1 -PackageDirectory C:\Games\FreeRiders -Mode fast
```

The launcher retains its normal settings and 60 FPS cap. Play, then close the
game and launcher. Each invocation saves `game.log`, the final `settings.ini`,
executable hash and host information into a new `benchmarks` directory inside
that package. Inherited `SFR_*` overrides are recorded for diagnosis; a clean
PowerShell session is preferable. The comparison switches are `SFR_GPU_WAIT_FAST=0/1`, `SFR_MAIN_AFFINITY=0/1`
and `SFR_WORKER_AFFINITY=0/1`, set together by the script;
intrusive sampling profilers are disabled during capture. The script restores
its process environment afterward. Do not run two instances simultaneously.
Capture explicitly sets `SFR_FRAME_METRICS=1`; ordinary launcher play skips
detailed draw timers/stream collections and emits a bounded heartbeat instead
of consecutive frame records. `SFR_TRACE_INPUT=1` restores packet-change logs
when diagnosing controls; benchmarks use `0` and retain connection changes.
When comparing different releases instead, use their normal defaults and label
the executable versions; older releases ignore these switches. The legacy mode
keeps the shader lifetime, posture and zero-timeout WaitAll correctness fixes;
it restores the old wait-release and Windows host-thread placement policies.
For isolated placement comparisons, leave GPU fast waits enabled in both runs
and change only the two affinity switches. Explicit thread/process CPU Sets
are respected by leaving automatic placement unchanged.

## Analyze the same scene

Find the `frame=` numbers for the measured portion of each log and run:

```text
python scripts/benchmark_frames.py path/to/game.log --first 13001 --last 13999 --output result.json
```

Menu/loading duration can differ even with scripted input. Absolute frame numbers
therefore do not guarantee matching race scenes. For a verified course where the
first frame above 600 draws identifies the same scene transition, this optional heuristic
aligns each log independently:

```text
python scripts/benchmark_frames.py old.log new.log --scene-draws 600 --first 800 --last 1200
```

Check screenshots, HUD time and draw-count distributions before accepting that
alignment. This threshold is **not a universal race detector**. AI collisions,
different items and random events can still change workload after alignment.
In the tested Team Heroes course it identifies the mission introduction, so
the early offsets still include pre-race scenes; the example's later window
was checked against race screenshots. Verify it again for your own capture.
Repeat runs; report individual results and variation, not just the best result.

Useful measurements:

| Value | Meaning |
| --- | --- |
| `fps` | Frames divided by elapsed time across consecutive intervals, not averaged instantaneous FPS. |
| `frame_ms` p50 / p95 / p99 | Typical frame time and slower-frame tails; lower is better. |
| `non_pacing_ms` | Wall time outside the frame-cap wait; includes CPU work, GPU/OS/guest scheduling waits and logging, **not pure CPU or GPU time**. Available in new logs. |
| `pacing_ms` | Frame-cap wait including reacquisition of the guest execution permit. |
| `draw_ms`, `pipeline_ms` | CPU-side draw work and pipeline creation. These overlap; do not add them. |
| `gpu_wait_ms` | Time inside the GPU wait wrapper, including permit reacquisition; not a GPU timestamp. Fast-completed waits intentionally bypass this wrapper. |
| `main_queued_ms`, `main_ready_unowned_ms` | Guest execution admission waits; help distinguish scheduling stalls from graphics work. |
| `complete_window`, `missing_intervals`, `stops` | Reject incomplete windows and unexpected failures. An intentional `present-limit` is normal for automated runs. |

The over-16.667-ms count includes normal timer jitter around a 60 FPS cap; do not
equate it directly with visibly dropped frames. At a stable 60 FPS, compare
non-pacing time and p95/p99 to estimate spare capacity instead of raising the cap.
Also compare a real stopwatch against HUD race time. A game that presents below
60 FPS currently runs slower; the optimization must not change the 1/60 step to
make a throughput result look better.

## Interpreting a 4090 result

A fast GPU does not remove CPU emulation, memory or scheduling costs. Improvements
to those costs can be measured here, but they do not establish an FPS gain on
an Android/Mali device or a slower CPU. CPU affinity/power restrictions are stress
tests, not faithful substitutes for different hardware. Keep the reproducible
logs and repeat this procedure on the affected device before claiming its issue
is resolved. A lower output-window size alone does not reduce internal rendering
work; use the separate [Rendering resolution](internal-resolution.md) setting.
Keep it identical across CPU-optimization comparisons and vary it separately
when investigating a GPU bottleneck.

Guided isolated configurations for 1P, 2P, Camera and VRM are described in
[Benchmark scenarios](benchmark-scenarios.md). These record configuration and
require scene/feature verification before comparing results.

The [performance/resolution validation report](performance-resolution-validation.md)
records the subsequent local build's verification separately from earlier
placement measurements.
