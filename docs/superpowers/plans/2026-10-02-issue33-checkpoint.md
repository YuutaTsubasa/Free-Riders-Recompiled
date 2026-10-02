# Issue #33 checkpoint validation

User request: investigate Issue #33 and use its detailed measurements to improve
performance. Continue in camera-debug; no public comments, push or release.
Execute serially without sub-agents. Preserve main checkout and other work.

## Decision

Prioritize the small checkpoint interval change. The reported six paired rounds
on each of three Windows CPUs consistently favor 256 over 32, but use an older
fork, D3D12 and disabled audio. Current runtime defaults remain 32 until current
measurements and functional regressions support a platform-specific decision.
Do not import the separate render thread or unchecked memory changes together.

The experimental control accepts a complete unsigned integer in 1..4096 and
falls back to 32 otherwise. Read once in the slow permit path and log the chosen
value. Generated entry/loop code stays unchanged. Larger intervals can delay
cancellation and urgent handoff; entry counts are not wall-clock guarantees.

## Checks and evidence

- [x] Save exact issue and fork branch revisions; recompute paired ratios.
- [x] Review scheduler cancellation, urgent handoff and quantum sampling.
- [x] Red/green parser, actual entry/loop cadence, real lease cancellation tests.
- [x] Existing scheduler plus entry-state targets pass; Windows/Android build.
- [x] Windows normal-time/audio D3D12 Frozen Forest at 256 reaches Replay.
      36000-present limit, 602.578s; source save unchanged; functional, not FPS.
- [x] Remove the separate rejected depthless-framebuffer candidate before the
      checkpoint Android comparison. Rebuild clean Windows executable.
- [x] Same private APK on Thor: 32/256/32, normal elapsed time, audio, 720p,
      bounded 15000 presents, independent fixture copies, exact ordinary APK
      restoration and no debug override after each case. Check screenshots,
      log hashes and CPU/GPU/frame measurements in a matched HUD window.
- [x] Decide whether the device evidence exceeds baseline variation; otherwise
      keep the experimental override and default 32. Do not infer an Android
      result from the Windows report or a gain from a 60-FPS-capped desktop run.
- [ ] For Windows throughput, adapt/review elapsed-time menu pacing separately
      before an uncapped A/B run. Preserve audio, guest time and save isolation.
      Do not execute the fork harness's shared-cache cleanup unreviewed.
- [ ] Update performance documentation and continuation handoff. Local commit
      only for verified work, credit Issue #33 / knuckleslee's measurements.

Evidence: ignored `out/continuation-20261002/issue33`,
`checkpoint-clean-probe`, `android-checkpoint-clean-probe`,
`thor-checkpoint-clean-32a`, `thor-checkpoint-clean-256`,
`thor-checkpoint-clean-32b`. The private Android runner restores the known normal
APK after every run and verifies its hash. Ordinary settings are never rewritten.

Thor result: 20.249/23.681/23.345 FPS, but draw workloads1517/950/866 differ.
Candidate versus later baseline+1.44%, baseline variation15.3%; no reproducible
Android benefit established. All restoration checks pass. Default32 retained.
Windows current-version uncapped measurement remains pending; no blanket gain
or completed performance optimization claimed.
