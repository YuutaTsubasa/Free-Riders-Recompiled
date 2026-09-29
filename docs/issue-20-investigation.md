# Issue 20: crash, posture and performance

Investigation against v0.4.0 (`5e07175`), September 30, 2026.
Issue: <https://github.com/YuutaTsubasa/Free-Riders-Recompiled/issues/20>.

## Reproduced failure and correction

The supplied crash screenshot stops with `native shader or original object is
already attached`. A shader constructor can allocate a new guest object at an
address previously occupied by another shader. Native shader stages and pipelines
outlive that allocation; treating the guest address as a permanent unique owner
incorrectly rejects the new object.

Native stages are now deduplicated by the stable translated shader entry. A
successful original constructor can replace the association at its newly
allocated guest address. The native stage remains alive for existing pipelines
and submitted work. A regression using the release shader pack covers shared
stages, address reuse by a different shader and 2,100 repeated loads without
exhausting the 2,048-stage limit. It reproduced the old failure and passes with
both D3D12 and Vulkan after the fix. This addresses the reported stop, not every
possible crash on the reporter's hardware.

## Controller posture

The controller adapter stores action values in the synthetic body record. The
game's live Kinect overlay also reads overlapping fields as joint coordinates,
which can twist a controller-driven rider over its authored animation.

The correction suppresses only seven identified live-tracking joint rotation
writes for a validated local controller rider. It preserves authored animation,
root motion, stance changes and action detectors. The hook follows the same
latched player routing as the existing controls. Real Kinect, camera-controlled
players, AI riders and unrelated joint writes retain their original path.

Hook tests cover 1P/2P, swapped/shared routing, camera ownership, connection
changes, partially initialized lists and retired riders. Local runtime captures
show authored riding, held-item and stance-change/jump poses. Physical two-player
and camera gameplay, and the reporter's exact character/gear combination, still
need user validation.

## Performance and timing

The game advances roughly 1/60 second per presented frame. This change keeps the
normal 60 FPS cap and the existing simulation clock. Uncapping is not a valid
real-time performance test: it accelerates game logic. Conversely, a run below
60 FPS still slows down; this work does not decouple simulation and rendering.

Completed GPU fences can now finish without releasing and reacquiring guest
execution. D3D12 consumes its auto-reset fence event once; Vulkan checks the
fence and then performs the existing reset. Pending fences use the original
guarded wait. Tests exercise repeated fence reuse, rendered readback, the
legacy switch and a pending-query fallback.

The investigation also found a Windows zero-timeout `WaitAll` bug: it returned
timeout before checking an already-ready set. The corrected native poll consumes
the whole set atomically. Tests verify no partial consumption and cancellation.

### Windows CPU placement

Further sampling on the i9-14900KF / RTX 4090 identified two scheduling costs.
The main guest thread is the original process thread, so the existing worker
placement never applied to it. All 1,601 samples in the diagnostic race window
found it on efficiency cores. Pinning just this thread to an allowed performance
core improved that diagnostic run from 30.51 to 40.45 FPS. A second experiment
allowed worker 16 to move between performance cores and reached 50.29 FPS;
its time waiting at the front of the guest queue fell sharply.

The production fix places the main thread using the existing physical-core
ordering and lets workers migrate within the first six allowed host processors.
Each worker retains its original preferred processor. Machines with fewer than
six available processors use that smaller pool. Guest processor identity, guest
execution locks and simulation timing are unchanged. Explicit Windows CPU Sets
are respected by skipping automatic placement; the main thread also retains any
inherited hard-affinity restriction. No system power settings are changed.

`SFR_MAIN_AFFINITY=0` and `SFR_WORKER_AFFINITY=0` independently restore the old
placement for comparison. Other platforms retain their existing placement.
Tests check actual OS affinity, preferred processors, one/two-processor process
restrictions, explicit CPU Sets, invalid requests and guest affinity bookkeeping.

An experiment that polled CPU events before releasing execution passed its
correctness tests but increased early-race stalls in the game measurement. It
was removed, as was an earlier condition-variable wake-up experiment without a
demonstrated gain. Neither is included in the delivered runtime.
A measured gain on this CPU does not establish a gain on a slower CPU or Android/Mali.
Use the [repeatable measurement procedure](benchmarking.md), with matching
course scenes and repeated runs, before making a device-specific claim.

### Initial GPU-wait measurements

Windows, i9-14900KF / RTX 4090, one standard rider, 1280 x 720, every frame
rendered, 60 FPS cap, camera/VRM/audio off. Vulkan runs start from the same
warmed pipeline cache and save. The measured interval is 400 consecutive
frames at offsets 800..1200 from the first frame above 600 draw calls. That
threshold marks the mission introduction; later race screenshots confirm the
selected window is gameplay. AI and item variations still affect the workload.

| Vulkan run | FPS | Mean frame ms | p95 ms | p99 ms | Mean draws | GPU wait wrapper ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU fast wait | 32.01 | 31.24 | 48.91 | 64.67 | 824.38 | 0.000 |
| Legacy wait, same executable | 30.61 | 32.67 | 48.32 | 57.49 | 817.00 | 0.574 |

These are one pair, not a statistically established speedup: average throughput
improved but the slower-frame tails did not. The defensible result is removal
of redundant completed-fence wait overhead, not a guaranteed FPS increase.
The executable for this pair is SHA-256
`649a02044a0765da7dc3fc435ba7f3d2b26dbd8912f238fcb5345f50d42d97c7`.
It predates the independent zero-timeout WaitAll correction.

The discarded CPU polling experiment measured 23.13 FPS / 94.98 ms p95 in the
same early-race window, versus 29.16 FPS / 55.80 ms p95 with both wait
optimizations disabled in that experimental executable. These measurements
did not justify enabling the CPU experiment. Raw logs, captures, executable
hashes and launch environments are retained locally under `out/issue-20/`.

The earlier executable (`9eee4000b0d8e4291a942fb789101becf11940a9809e61d32a60fbff2c25308e`)
completed a D3D12 run to 11,500 presents. Its checked early-race window measured
42.86 FPS, 23.33 ms mean, 38.73 ms p95 and 50.35 ms p99, with 817.00 mean draws.
This single final-build run has no matched D3D12 legacy pair and cannot be used
to attribute the difference from earlier runs to this patch. It confirmed a
working runtime, not a general D3D12 speedup.

### Matched Windows placement comparison

Same machine, Vulkan, warmed cache, save and settings as above. Both runs use
the same executable with GPU fast waits enabled. Only `SFR_MAIN_AFFINITY` and
`SFR_WORKER_AFFINITY` change together. The wider measurement covers 1,600
consecutive intervals at scene offsets 800..2400; screenshots verify the same
race segment. Mean draw counts differ by under 1%, although AI/items are not
deterministic.

| Placement | FPS | Mean frame ms | p95 ms | p99 ms | Mean draws | Guest queue ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Legacy | 28.92 | 34.58 | 54.08 | 65.47 | 873.16 | 9.12 |
| New | 54.67 | 18.29 | 22.30 | 26.82 | 879.51 | 1.48 |

The shorter 400-interval window measures 30.76 versus 60.00 FPS. The broader
window avoids presenting this as a constant 60 FPS result. HUD time advances
from 15.558 to 32.225 seconds over 1,000 presents in the new run: the original
1/60 simulation step is retained. Below 60 FPS the game still slows down.

These runs are `placement-legacy-vulkan-a` and `placement-vulkan-a`, executable
SHA-256 `0d39ea7a4ef1865d54b6111b80bc7d94708f4ed75f4d403313f34e7d5d439c10`.
This diagnostic executable includes queue-front attribution used to identify
the blocked worker. That extra accounting is removed from the delivered build;
the existing scheduler itself is unchanged. The measured improvement is specific
to this Windows CPU and workload, not a promised speedup on every device.

The first local test executable, SHA-256
`15a54060ffb842e94f5c259339dbd1164d89e8d4d5f52b437bbd0afcbecfc8ca`,
repeats the Vulkan test without the extra queue-front counters. Run
`placement-final-vulkan-b` measures 57.37 FPS, 17.43 ms mean, 20.03 ms p95,
21.18 ms p99 and 844.89 mean draws in the same 1,600-interval window. This
confirms the improvement survives in the delivered build; the different AI
workload and executable prevent attributing its additional gain to removal of
the counters. Both new runs reach 60 FPS in the shorter early-race window.

The same delivered build also completed `placement-final-d3d12-audio` with
XAudio2 output enabled, reaching its intentional 14,500-present stop. The aligned
1,600-interval window measured 54.23 FPS, 18.44 ms mean, 22.00 ms p95 and 24.44 ms
p99 (873.55 mean draws). The game reached the mission-failure screen normally
because the scripted rider did not collect enough rings. There was no shader
attachment stop or process crash. This is a D3D12 runtime check with audio, not
a matched D3D12 legacy comparison or a subjective audio-quality assessment.

## Resolution follow-up

The measurements above use native 1280 x 720. The subsequent user-approved work
adds a separate [internal rendering resolution](internal-resolution.md) setting
from 360p to 1440p; output-window size remains independent. Those changes require
a new shader ABI and matching pack. See the separate
[follow-up validation](performance-resolution-validation.md) for the current
local build and its limitations. Do not treat old output-size changes as
internal-resolution measurements or combine different builds' measurements.

## First local build validation scope

The first local runtime passed all 114 Windows C++ tests, Python's 248-test suite
(11 skipped), and four targeted Linux synchronization/input tests. The real
shader-pack reload regression also passes on both D3D12 and Vulkan.
Local D3D12/Vulkan game runs have been exercised. Local game runs end at the
intentional present limit. No affected low-end device was available, so the issue
should remain open pending confirmation of the crash and posture on that device.
The local test package is not a published release.
