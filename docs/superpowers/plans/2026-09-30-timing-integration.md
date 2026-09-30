# Claude timing integration and comparison plan

User authorization: cooperate without modifying Claude's checkout, improve
handheld performance autonomously, preserve game/audio time. The previous turn
made progress: aee1d56 adds measured opt-in constant upload reuse.

Design: import eafeba9 then 567ab89 into the existing Codex checkout, retaining
original authorship. Resolve guest_threads suspension conflict by keeping the
notification path; precise_sleep only replaces the legacy comparison poll.
Do not enable unrelated constant reuse while measuring timing and priority.
Existing SFR_TIMER_RESOLUTION remains an independent, explicitly disabled
benchmark knob. Update source/docs that currently assert an unproven Ally
root cause or equate +16 with pinned Canary's mapping.

- [x] Import commits separately. Inspect conflict and resulting diff, preserving
      our notification tests and diagnostic tracing. Keep unrelated plan intact.
- [x] Build Windows runtime and affected tests; exercise HOST_TIMING=0 and 1
      in separate processes and SATURATED_PRIORITY=0 and 1 independently.
      Add test coverage of opt-out behavior if existing tests assume enabled.
- [x] Validate elapsed waits and fallback without a scheduler upper-latency
      assertion: the OS can legitimately delay a ready thread on loaded hosts.
      Add explicit reporting when policy fallback only accepts execution-speed.
- [x] Use existing native timer-resolution lifetime owner instead of an unmatched
      timeBeginPeriod call; test request/release behavior through existing owner.
- [x] Build Android targets for portable fallback, then same-executable desktop
      race comparisons: all off, timing on/priority off, both on, all off.
      Keep notify=1, timer=0, constant reuse=0, Vulkan/720p/60 FPS/audio and
      11,500 presents, no profiler or overlapping game/build. Compare late
      9,500..11,000 windows and actual HOST_TIMING diagnostics.
- [x] Check full Intro on retained configuration (4,000 presents, resting hand)
      and full Windows suite. Review before committing follow-up corrections.
- [x] Record exact binary, settings and results in report/shared handoff; choose
      defaults from evidence. Desktop cannot establish handheld gains or Xenia
      superiority. Do not replace test2 packages or publish a release.

Validation scope: fallback paths were source-reviewed and cross-compiled for
Android; Windows timer API failures were not fault-injected, and Android tests
were not executed on a device. The elapsed-wait tests ran on this Windows host.
Desktop comparisons completed; they do not satisfy handheld acceptance.
