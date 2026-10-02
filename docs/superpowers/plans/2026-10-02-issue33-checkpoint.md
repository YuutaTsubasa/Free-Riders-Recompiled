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
- [x] For Windows throughput, adapt/review elapsed-time menu pacing separately
      before an uncapped A/B run. Preserve audio, guest time and save isolation.
      Do not execute the fork harness's shared-cache cleanup unreviewed.
- [x] Update performance documentation and continuation handoff. Local commit
      only for verified work, credit Issue #33 / knuckleslee's measurements.

Evidence: ignored `out/continuation-20261002/issue33`,
`checkpoint-clean-probe`, `android-checkpoint-clean-probe`,
`thor-checkpoint-clean-32a`, `thor-checkpoint-clean-256`,
`thor-checkpoint-clean-32b`. The private Android runner restores the known normal
APK after every run and verifies its hash. Ordinary settings are never rewritten.

Thor result: 20.249/23.681/23.345 FPS, but draw workloads1517/950/866 differ.
Candidate versus later baseline+1.44%, baseline variation15.3%; no reproducible
Android benefit established. All restoration checks pass. Default32 retained.
Windows uncapped measurement is complete using a private cap60 menu path until
13200p, then120seconds uncapped with normal guest time/audio. Three runs80.931/
81.305/75.165FPS, draws841/902/917, baselinevariation7.67%; no repeatablegain
established. HUD and audio-submission timing verified, allownedprocessesended,
sourcefixture/normaloutputs/settingsunchanged. This scoped measurement pass is
complete; productiondefault32 remains. Additional paired/comparable-workload or
slower-host validation is follow-up, not a demonstrated performance fix.

## Follow-up: stationary workload, completed 2026-10-02 13:57 UTC

Ordinary continuous brake from12000p holds the start-line position without a
game-state patch. Same private executable32/256/32, audio/elapsed time/all draws,
120s uncapped window, independent fixtures/caches; nine screenshots inspected.
HUD45–80 throughput86.923/90.162/87.380FPS, draws670.0/678.8/669.7; candidate
+3.73/+3.18percent versus baselines, baseline variation0.53percent. Other
preselected35–55/55–75 windows also positive2.80–4.24percent. Positive stationary
desktop evidence, not replicated active-race/low-end/Android proof. Default32
retained. HUDoffset spread<=0.017s, no selectedclockclamps/>50msframes, audio
cadence187.48–187.52blocks/s. All bounded exits and source-save checks pass;
ordinary source/object/executable hashes unchanged and no gameprocess remains.

Next checkpoint work should replicate this controlled workload with the order
reversed and cover responsiveness before a Windows-specific default decision.
Do not repeat uncontrolled autonomous-route comparisons or infer Android gains.
No runtime code change, external comment, push or release in this follow-up.

## Reverse-order replication and urgent handoff follow-up

- [x] Run 256/32/256 with new isolated case names and the same stationary
      workload/executable; preserve the first sequence and compare all three
      predefined HUD windows. Inspect fresh screenshot/time anchors.
- [x] Extend the existing entry-state test to exercise real urgent handoff at
      the next inline entry interval (1/32/256/4096), with an unexpired quantum.
      Verify a private delayed-handoff mutant fails this assertion, then verify
      the unchanged production scheduler passes both existing test targets.
      Compile only after performance runs end to avoid workload interference.
- [x] Record results, limitations, source hashes and ordinary-launch cleanup.
      Entry-count tests do not establish a wall-clock input/audio latency bound.
      Keep the production default unchanged during this validation.

Completed 14:27 UTC: reverse256/32/256 gives92.877/87.278/91.517FPS inHUD45–80,
draws667.6/667.2/670.0, candidate+6.42/+4.86percent. Allpredefinedwindowspositive.
Allthree342.141s bounded exits, correctHUDclock, sourcefixturespreserved; sixcase
cross-checks validate identicalbinary/settings/fixtures. Ordinaryoutputsunchanged,
noownedgamesremain. Private delayed-urgentmutant failsnewassertion; production
guest_entry_state+guest_execution2/2pass3.27s. Onlytest/documentationchanges.
Next: Vulkan throughput and activeinput/audio beforeWindowsdefaultdecision;
no broaderlatency/Android/Issue31fixclaim. Default32retained, nopublication.

## Vulkan comparison and private functional observer

- [x] Repeat the same stationary 32/256/32 protocol using Vulkan and the same
      bounded executable. Inspect fresh HUD anchors and all preselected windows.
- [x] Prepare a private observer for wall-clock scripted controls, original lean
      consumer transitions, XAudio2 health and nonfinite audio samples. Build
      only after throughput runs finish; preserve ordinary sources/objects.
- [x] Record current evidence and remaining limitations. Private observer builds
      are preparation, not proof of input/audio behavior until their runs pass.

Observer design and API references are in ignored
`out/continuation-20261002/issue33/input-audio-probe-design.md`. No production
default or audio/input behavior change is part of this measurement step.

Completed 15:15 UTC: Vulkan stationary32/256/32 gives78.474/80.585/77.257FPS
(HUD45–80), candidate+2.69/+4.31percent with approximately matched draws.
Both other predefined windows are positive. Separate same-binary private Vulkan
input/audio32/256 runs end342.329/342.141s; all18 scripted transitions reach both
original consumers with correct signs/scales/releases, within one present of
first logged polling. Nominal-to-actor maxima15.393/14.003ms. Audio121reports
percase/~120.26s: zero engine-glitch increments, empty/full queue observations,
submission failures and nonfinite samples. Not physical HID/display latency or
audible-quality proof. Scientific-notation parser corrected after the initial
1e-07 sample exposed a parsing error; raw evidence retained, clock check passes.
Fresh verification confirms case binaries/settings/fixtures, unchanged ordinary
source/object/executable hashes, and no game process. Default32 retained.
Next: D3D12 active input/audio and slower Windows host when available. Do not
repeat completed stationary sequences or infer Android gains. No publication.
