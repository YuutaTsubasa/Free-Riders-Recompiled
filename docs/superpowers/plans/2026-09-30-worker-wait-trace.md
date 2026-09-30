# Selected worker wait trace

Goal: identify why the main thread still waits ~1.4 ms on worker 7 after
notification wakeups, without changing scheduling or Claude's timing worktree.
User has authorized autonomous joint performance research.

Design: keep existing main-thread collection. With SFR_WAIT_TRACE=1 and an
explicit SFR_WAIT_TRACE_GUEST > 1, collect only that worker's waits in its own
thread-local collector. The worker flushes its own collector after a completed
wait every five seconds, tagging all rows with guest_id. No main/worker shared
collector or hot-path locking. The last partial worker window is not guaranteed
to flush at exit; analyses use complete reported windows. Ordinary play remains
uninstrumented. Use existing observe_wait runner semantics.

Steps:
- Test guest identification and independent collector reset.
- Add optional writer guest ID and selected worker integration.
- Build and run wait trace tests; run a worker-7 race capture while no other
  game is running. Preserve default 60 FPS/game clock and record overlap.
- Analyze worker wait costs and report evidence to the shared handoff file.
- Keep hypotheses distinct from handheld-verified improvements.

Completed: selected-worker writer/ownership tests failed before implementation
and pass after it; Windows runtime built; wait_trace and guest_threads CTest
passed (2/2). Worker wait and host-instruction captures each reached 11,500
presents, zero overlap and zero collector overflow. Evidence and uncertainty
are recorded in docs/handheld-performance-2026-09-30.md and the shared handoff.
The original handheld-smoothness/Xenia goal remains unverified and active.
