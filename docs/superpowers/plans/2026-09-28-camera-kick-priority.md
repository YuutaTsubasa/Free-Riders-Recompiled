# Camera Kick Dash priority regression

The user means the Air-consuming one-leg Kick Dash, not ordinary forward board acceleration. Earlier guidance incorrectly called the calibrated forward acceleration Boost. Correct the documentation and label the numeric motion log `acceleration`; preserve the existing leg gesture rather than inventing a torso substitute.

Evidence: latest actual-race trace (2026-09-28 00:53) has 14 Camera 822CA518 successes, while P1's consumer ring 0xFEF28A00 has no 0x800000/0x1000000 kick bits. Original group 822C5BB0 removes 0x1800100 unless shared priority bit 0x400000 is set. Camera's false-brake Side guard suppressed that prerequisite; the controller path already supplies it with kick events. The same actual-race consumer does show crouch animation 19/state 6, so do not change crouch/jump based on the earlier restricted tutorial replay.

Fix: only after an ungated original Camera Kick/Dash detector, if its result entry contains kick preparation or release, supply the shared 0x400000 prerequisite. Never manufacture kick events or brake bit 0x100. Hand-action and jump guards bypass the original detector and this completion step. P2 and controller paths stay unchanged.

Regression: actual production hooks plus the original group filter contract; preparation and release retain native return values, survive filtering, do not produce brake, no-event input gains no priority, and P2 is isolated. Waving continues to suppress native kick state and priority. Test failed specifically at the group filter before the fix and passes after. Full relevant suites and local build required before staging.
