# Handheld wait tracing

The v0.4.2 Ally X capture reaches roughly 32 FPS in its late high-draw window.
This opt-in diagnostic adds attribution, not a performance fix. It does not
change the 60 FPS cap, guest time steps, wait timeouts or execution policy.

Set `SFR_WAIT_TRACE=1` before starting the runtime, normally through the bundled
`Capture-Waits.cmd` / `scripts/capture_wait_trace.ps1`. Also enable
`SFR_FRAME_METRICS=1` for frame alignment. Close both game and launcher after
watching the Intro for 20–30 seconds and playing the affected course for about
two minutes. The capture ZIP includes the game log, settings and limited host /
user-supplied power-mode metadata. It excludes game data, saves and models.

`WAIT_TRACE_WINDOW` bounds each approximately five-second window and reports
the last present index. Final shutdown flushes a partial window. `WAIT_TRACE`
groups guest 1's waits by kind, caller address, full target list, critical-section
owner **guest object**, native timeout and status. Times are sums, except
`max_native_ms`; divide by `count` for per-call means:

- `before_ms`: entry into the wait wrapper until its native callback starts.
- `native_ms`: callback start until return; includes OS scheduling and blocking.
- `resume_ms`: callback return until the runner returns, including execution
  permit reacquisition and post-wait bookkeeping. It does not timestamp the
  event signal or prove the exact runnable-to-running delay.
- `overshoot_ms`: excess native time over the requested duration, only for
  actual timeout results, explicit delay and 50/100 microsecond polling sleeps.
  Successful event waits are not labelled oversleep.
- `requested_min_ns` / `requested_max_ns`: original requested duration range.
  `frame_cap` uses a stable aggregation key (`timeout_ns=-1`) because its
  remaining deadline changes every frame. Other `-1` values mean unknown or
  infinite. All-wait targets are listed together; no individual target is
  falsely designated as the blocker.
- `failed`: operation/runner exceptions, including cancellation. They remain
  exceptions in the game; the observer neither swallows nor retries them.

The collector has 128 slots per window. `dropped` and dropped total/native/resume
times make saturation visible. A saturated window cannot fully attribute every
wait, even though overflow time is preserved. The data is owned by guest 1;
workers do not write this collector. Disabled mode bypasses its clock reads,
aggregation and logging.

For producer-side diagnosis, additionally set `SFR_WAIT_TRACE_GUEST=7` (or
another guest ID greater than one). The selected worker gets a separate
thread-local collector and flushes it after a completed wait approximately
every five seconds. Both window and detail lines include `guest_id`; existing
main-thread collection is `guest_id=1`. Group analysis by that field: worker
and main waits overlap and must not be added together as frame time. The
worker's final partial window may not flush when it exits or waits indefinitely.
Malformed, zero or one selections add no worker collector. This switch requires
`SFR_WAIT_TRACE=1` and is off in normal captures.

The older `WAIT_GRAPH` requires a separate `SFR_WAIT_GRAPH=1` opt-in; its worker
signal mutex/map updates add overhead and are disabled in normal wait captures.
`setters` contains observed
guest signal-producer counts and is only a **correlation hint**: asynchronous
host producers may be absent, multiple waits use the first target in that older
summary, and a counted signal need not have satisfied the observed wait. Its
`main_ms` includes permit reacquisition and must not be added to `native_ms`.

`MOVIE_TRACE` records the XMV RenderNextFrame call path every 30 calls or two seconds:
call count, success returns, summed/maximum call duration, present index and host
monotonic time. Success returns are not an independently verified decoded-frame
counter. Match these with the audio pump's `NATIVE_AUDIO_FRAMES` and the wait
rows; present FPS alone can include unchanged movie frames. The existing movie
compatibility patch uses blocking RenderNextFrame to avoid black frames when
the guest decoder has not produced its next frame. This diagnostic retains it.

Tracing adds overhead. Compare on/off with the same binary, settings, course,
cache conditions and power mode; local desktop measurements do not establish
Ally X performance. These counters are not GPU timestamps or CPU samples.

## Candidate runtime switches

The accompanying handheld candidate also changes guest suspension to a
stop-aware notification wait. Default behavior is notification; set
`SFR_SUSPEND_NOTIFY=0` to use the former 1 ms polling wait in the same binary.
Nested suspension counts, cancellation and resume-before-wait semantics stay
the same. This comparison isolates the wait strategy, not every change since
v0.4.2. Keep graphics, course, power and cache conditions consistent.

`SFR_TIMER_RESOLUTION=1` requests a scoped Windows 1 ms timer period. It is
**off by default**: desktop calibration did not establish a benefit. It is a
separate experiment, not required by the notification wait. A successful
`TIMER_RESOLUTION active=1` means the request succeeded, not that every sleep
will meet its deadline. Non-Windows platforms do not request a timer period.

Normal play (`SFR_TRACE_IMPORTS=0`, as set by the launcher) now skips file-read
fingerprints and allocation/protection diagnostic bookkeeping. Import tracing
still enables those diagnostics. Shader content validation at startup remains
unconditional. Pipeline keys also serialize semantic state without structure
padding; the on-disk driver cache and shader-pack ABI remain compatible.
