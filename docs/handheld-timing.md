# Handheld timing: whole timer ticks on the Ally X

The v0.4.2 capture from a ROG Xbox Ally X (Ryzen Z2 Extreme, Windows 11,
1280x720 windowed, 60 fps cap) ran a 1P race at 32 fps. Its frames were
not slow in general; they were quantized:

| Frames | Median interval | Most frames last |
| --- | ---: | --- |
| menus and intro (1–2500) | 15.8–16.1 ms | 1 × 15.625 ms |
| race (3000–7418) | 31.2–31.5 ms | 2 × 15.625 ms |

15.625 ms is the Windows default timer tick. In the race window the main guest
thread ran for 11.5 ms per frame and drew in about 3 ms, close to a desktop
(12.2 ms on an i9-14900KF), but was blocked for 18.9 ms of which only 0.5 ms
had a busy guest thread to blame. Nothing was working: somebody was asleep.

Every short sleep in the runtime (`sleep_for(1 ms)` in the frame cap and the
self-suspend poll, the 50/100 µs job polls, guest `KeDelayExecutionThread`)
lasts a whole tick when the process's timer resolution is 15.625 ms. SDL
already asks for 1 ms (`timeBeginPeriod`), so on that device Windows ignored
the request. Windows 11 does that for processes it power-throttles
(`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`, part of EcoQoS, which
also prefers efficiency cores and lower clocks). A desktop on mains power
does not show it: `sleep_for(1 ms)` takes about 1.5 ms there with or without
any request.

## What the game process does now

`configure_host_timing()` (src/host_timing.cpp) runs first in `main`:

- `SetProcessInformation(ProcessPowerThrottling)` with `ControlMask =
  EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION` and `StateMask = 0`: the
  documented way to say "never throttle this process, always honour its timer
  resolution". Older Windows 10 takes the execution-speed half only.
- `timeBeginPeriod(1)`, held for the life of the process.
- It measures four `sleep_for(1 ms)` and four `precise_sleep(1 ms)` and logs
  one line, for example on the desktop:

  ```
  HOST_TIMING throttling_opt_out=1 timer_period=1 high_resolution_timer=1 sleep_1ms_ms=1.36 precise_1ms_ms=1.50
  ```

  A capture whose `sleep_1ms_ms` is near 15.6 still has the problem; one near
  1–2 does not.

`precise_sleep()` waits on a per-thread `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`
timer, which does not depend on the process timer resolution at all. The
self-suspend poll (`GuestThreads::suspension_waiter`) uses it. The other short
sleeps are the same kind of candidate; they sit in lines the wait-tracing work
(`SFR_WAIT_TRACE`, docs/wait-tracing.md) is changing, so they are left for
after that lands.

`SFR_HOST_TIMING=0` skips the opt-out and `timeBeginPeriod`, for comparison
runs; the log line is still written.

## Checking it on the device

Run the launcher as usual with frame metrics (`SFR_FRAME_METRICS=1`) and look
for the `HOST_TIMING` line at the top of `game.log`, then at the race frame
intervals: the median should no longer sit on 31.25 ms. A desktop cannot
reproduce the ignored request, so only the handheld shows the effect.

## Also from reading xenia-canary

- **Saturated guest priority.** `KeSetBasePriorityThread` with +16 saturates a
  thread at the top of its range. The title gives it to guests 10, 17, 28 and
  30 and -2..2 to everything else, but our old mapping (±17/±34, Xenia's
  earlier one) put all of them at host Normal. +16 now maps to Above Normal
  (`SFR_GUEST_SATURATED_PRIORITY=0` restores the old mapping). On the i9
  desktop the race is unchanged (17.5 vs 17.3 ms, noise); it matters when a
  handheld's cores are busy.
- **Sub-100 µs sleeps yield in Xenia** (`xe::threading::Sleep`), and its yield
  is `NtDelayExecution(0)`, not `SwitchToThread`, which only hands the core to
  a thread already queued on the same processor. Our job polls sleep 50/100 µs
  with `sleep_for` (1.5 ms on the desktop, a whole tick on the Ally), and the
  frame cap's last 2 ms spins on `std::this_thread::yield()` with the main
  thread pinned to one processor: a busy loop that spends a handheld's power
  budget. Both belong to lines the wait-tracing branch is changing; switch
  them to `precise_sleep` after it lands.
