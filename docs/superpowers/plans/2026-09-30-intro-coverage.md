# Intro coverage investigation

Goal: explain the gap between a 72.414-second source movie and the short
movie trace in the existing desktop capture, without changing game timing.

Use the existing opt-in SFR_WAIT_TRACE movie hook in src/game_patches.cpp.
Record the first call and return-status transitions immediately, plus periodic
windows, with cumulative calls, successful returns, caller, incoming flags,
and elapsed time. Keep the skip policy and call arguments unchanged. A success
return is not evidence that a new video frame was decoded or displayed.

Steps:
- Preserve the unsuccessful vertex experiment and update the shared handoff.
- Add a capture verifier that rejects existing logs lacking first-call and
  status-transition evidence; establish that failure before the diagnostic edit.
- Extend only the opt-in movie log, build the runtime, and run a longer idle
  title capture with no scripted voice/buttons, allowing the attract movie.
- Verify actual movie activity, end/status changes, elapsed time and no overlap.
  Compare source metadata and guest call flow before inferring a timing defect.
- Document limitations. Do not claim handheld acceptance or ship a timing change.

A longer capture alone cannot identify why calls stopped; an immediate timing
patch would be speculative. This diagnostic step is the narrowest useful test.

Result: the race harness's pre-centred virtual hand was cancelling playback.
The trace showed an explicit game stop, then a one-variable experiment with
resting hands reached 72.4824 seconds against 72.414-second source metadata.
Private Intro runner corrected; runtime playback/timing policy unchanged.
Windows build, four targeted CTests and real-capture accounting pass.
Handheld speed and sample-accurate A/V sync are still unverified.
