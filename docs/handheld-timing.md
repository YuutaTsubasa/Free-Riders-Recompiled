# Handheld timing experiments

The v0.4.2 Ally X capture has race intervals near 31.28 ms, roughly 32 FPS,
with substantial time reported as main-thread blocked. This motivates testing
host timer policy and scheduling. It does **not** establish that short sleeps
lasted 15.625 ms, that the CPU/GPU was idle, or that Windows ignored a timer
request. We have no contemporaneous sleep-latency or power-policy measurement
from that device. Intro and menu frames must also be separated by actual movie
activity; present counts alone do not identify the scene.

## Host timing experiment

`configure_host_timing()` in `src/host_timing.cpp` runs before runtime workers.
On Windows it requests execution-speed throttling off and asks Windows to honor
process timer-resolution requests. If the combined policy is unsupported, it
retries with only execution-speed throttling disabled. The two outcomes are
reported separately as `throttling_opt_out` and `timer_resolution_opt_out`.
It also holds a 1 ms `NativeTimerResolution` request until process teardown,
where it is paired with `timeEndPeriod`.

Microsoft documents execution-speed policy and ignoring timer resolution as
separate flags. Their existence does not establish either was active on the
Ally. See [SetProcessInformation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation)
and [timeBeginPeriod](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod).

`SFR_HOST_TIMING=0` skips both policy changes and this timer-period request;
`1` enables them. It is currently enabled by default in the imported experiment.
The older `SFR_TIMER_RESOLUTION` option remains independent: set it to `0` for
these comparisons to avoid adding another request. SDL or other libraries may
also request timer resolution, so mode 0 is not a promise of coarse host ticks.

Both modes measure four ordinary 1 ms sleeps and four `precise_sleep(1 ms)`
calls and emit a `HOST_TIMING` line. Startup measurements are observations at
that moment, not guarantees of in-game scheduling latency. There is no fixed
upper-latency assertion in unit tests: a busy host can delay a ready thread.

`precise_sleep()` uses a thread-local high-resolution waitable timer on Windows,
falling back to `sleep_for` if creation, arming or waiting fails. It remains
available with `SFR_HOST_TIMING=0`; that switch controls process policy, not the
helper. Non-Windows builds use `sleep_for` and make no Windows policy requests.

The combined branch keeps Codex's stop-aware notification wait for guest
self-suspension (`SFR_SUSPEND_NOTIFY=1`, default). Only its legacy comparison
poll (`SFR_SUSPEND_NOTIFY=0`) uses `precise_sleep`. Other guest waits, frame
pacing deadlines and audio timing remain unchanged.

## Guest priority experiment

`SFR_GUEST_SATURATED_PRIORITY=1` raises guest increments 16 and 17 from host
normal to above-normal; `0` retains the previous mapping. Other boundaries and
negative increments are unchanged. This combined branch leaves it off by
default: desktop control runs vary enough that a stable benefit is unproven.
Explicitly set it to `1` to test the higher priority on the handheld.

Windows documents saturation for large increments to
[KeSetBasePriorityThread](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-kesetbaseprioritythread).
This is motivation for investigating title priorities, not proof that our
host mapping is an exact Xbox kernel implementation or that it improves FPS.
The pinned Xenia Canary revision maps 16 to normal and 17 to above-normal,
so this experiment is not identical to Canary. See the
[Xenia source comparison](xenia-performance-comparison-2026-09-30.md).

## Validation

CTest runs host timing enabled/disabled and guest priority enabled/disabled in
separate processes, alongside timer lifetime and notification/cancellation tests.
Record exact executable hashes, flags and scene timing for performance runs.
Compare timing on with priority off before adding priority, and repeat the off
control. Keep the graphics backend, resolution, frame cap, audio and suspension
mode identical. Full Intro playback checks duration separately from race FPS.

Desktop tests cannot establish handheld benefit. Device acceptance still needs
the same powered device, scene and settings, including a full Intro with audio.
Do not claim the handheld slowdown is fixed from a desktop sleep probe alone.
