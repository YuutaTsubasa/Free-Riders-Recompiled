# Race UI timing on Android

The user identifies the slow UI as the jump rating animation. Keep race physics,
input gesture timing, saves, and menus unchanged. This is a timing correction,
not evidence of a rendering throughput improvement.

## Evidence and investigation

- A read-only Thor probe finds `rank_in`, `rank_roll`, and `rank_stop` labels.
  The screenshot at present 11700 shows the large C rating and AIR +020.
- Three `rank_in` to `rank_roll` transitions in that run take exactly 82
  rendered frames, although race rendering is substantially below 60 FPS.
- The UI wrapper at 824AE2A0 evaluates timeline actions before calling
  824AE3E8 to advance. That routine adds the wrapper's fixed speed to its
  position and calls the timeline's next-frame operation at most once.
- 824AE728 advances nested timelines by one integer frame. 824A7EE0 evaluates
  actions matching the current timeline frame. Merely skipping ahead can
  therefore lose animation events.
- A second read-only probe correlates rating child timelines with their
  owning wrapper and records the actual speed and frame changes.

## Planned correction, conditional on the child probe

Use the already bounded real-time race step to update eligible UI wrappers in
steps of at most one original frame. Preserve authored playback speeds,
evaluate intermediate actions, stop at a completion notification, and restore
the wrapper's speed after each update. Only enable this for the known UI
timeline implementations while the real-time race clock is active. Keep an A/B
diagnostic override and retain the original path when real-time races are off.

## Validation

1. Model the original one-frame timeline behavior in the existing race clock
   test. Demonstrate the low-FPS failure before implementing bounded steps.
2. Cover fractional frames, authored half speed, completion, and bounded work.
3. Build Android and Windows, and run the timing tests on both available hosts.
4. Repeat the Thor fixture with matched read-only instrumentation. Compare
   rating timeline progression to the race clock, and inspect race screenshots.
5. Restore a normal, non-profileable APK and remove the diagnostic override.
   Report loading stutters separately; this change does not establish their fix.

## Cross-thread finding

The first candidate did not activate: `rank_in` to `rank_roll` still took
82¡V83 presents. A metadata probe showed the root wrapper uses the expected
821A9DD4 vtable, but runs on guest 16 while the race clock runs on guest 1.
Its thread-local UI delta remained 1. The correction publishes the completed
clock delta in an atomic scalar, resets to 1 on inactive/unsupported clock
updates, and snapshots it once per UI wrapper invocation. No transient reset
is published before an active clock update. Reentrancy remains thread-local.

The atomic does not synchronize guest object ownership; the original guest
scheduler and UI task ownership remain responsible for that. A source review
found no new actionable issues in this delta. Runtime validation is required.

## Validation outcome

- The original-step stub failed the UI duration regression; bounded substeps
  pass on Windows and on Thor's ARM64 runtime.
- Both production targets build. A second source review found no actionable
  issue in the shared-clock correction.
- The shared-clock fixture completes 14,400 presents. Three rating timelines
  reach the authored frame-79 hold in 37/34/42 presents, versus 79/79/82 in
  the original child probe. Approximate wall times are documented in
  `docs/performance.md` with the sampling limitations.
- `rank_in` to `rank_roll` is not solely animation time: the frame-79 pause
  waits for another game event. Keep that hold rather than forcing it away.
- The normal APK contains the production hook but none of the private rank
  trace strings. A final read-only loop/completion probe also completed 14,400 presents.
  The normal APK was restored successfully, debug.env was absent, and the
  launcher was reopened. Installed version: 0.4.5-perf-thor-ui-test.
