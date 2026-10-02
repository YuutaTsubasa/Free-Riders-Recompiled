# Issue 31: course stability investigation

This investigation uses independent save copies on a Windows RTX 4090 desktop,
ROG Xbox Ally X (Z2 Extreme / Radeon 890M), and AYN Thor (Adreno 740).
A successful launch is not counted as a finished race. Functional runs with
uncapped presentation and fixed one-frame simulation are accelerated tests,
not realtime performance measurements.

## Confirmed failures and corrections

### Loading: an actor is destroyed while a worker still uses it

Both Windows and Android reproduced a null access at `822A4C38`, after
`82918418` returned a valid skeleton record. The actor's source pointer at
`+324` changed to zero during that call. The adjacent six pointers were also
cleared, while the vtable remained intact: this matches destruction by
`822A42B0 -> 822A8EA8`, not a corrupt skeleton record.

A passive dispatch trace explains the timing window. Main's dispatch check
sees scene state 2 and skips waiting. The worker sees state 3 later in that
same frame and starts a batch. Next frame, deferred deletion runs before
main's next dispatch check. Depending on scheduling, the actor can therefore
be destroyed before a helper finishes its callback.

`JobLifetime` now protects batch admission, helper queue inspection and
nonempty deferred deletion. Helpers remain concurrent. Empty deletion lists
do not introduce a new main/worker barrier. Waiting releases guest execution
permits, and a timeout is not treated as permission to reset live jobs.
Tests cover a helper between queue polling and callback entry, pending
destruction versus a new batch, cancellation, exceptions and permit release.

Evidence lives under ignored `out/issue-31/frozen-desktop-epoch/` and
`out/thor-race-perf/magma-coverage.log`. The latter reproduces the same failure
in Magma Rift on the pre-fix APK, so this race is not specific to Frozen Forest.
It is a plausible cause of the reported crashes, not proof that every symptom
in the reporter's Issue 31 has the same origin.

### Frozen Forest: zero heading contaminates rotation and audio

A private Windows diagnostic forces two simulation frames per update after
present 13200. This reproduces the low-FPS ice-channel failure on the desktop
without relying on Android, fractional frame intervals or GPU load.

`8236DD60` receives `(0,0,0,1)` from the channel object's saved heading.
Its matrix is still finite and invertible. The transformed direction is zero;
`8225EE88` computes `0/0` while finding the angle between that direction and
forward. `82A54CF0` propagates NaN, and `82299420` passes that invalid angle to
both rider rotation interpolators. Movement, render transforms and the audio
mixer subsequently receive nonfinite values.

`race_orientation_hooks.cpp` retains the previous orientation only for an
exactly zero XYZ direction, including signed zero. Valid directions continue
through the original function. It does not clamp the shared math library or
replace invalid audio with silence. The regression first failed without the
hook, then passed with the guard, using the captured input.

The guarded 24000-present forced-step run recorded 155.061 seconds of raw guest
audio with **zero nonfinite samples** and no invalid rider rotation reports.
However, the rider still remained in the channel on lap 1 at 06:05.50. Thus
this correction prevents corruption; it does **not** establish a complete fix
for the low-FPS channel stall. The subsequent boarding investigation below
identified and corrected the remaining path/state failure.

### Frozen Forest: boarding pursuit ignores elapsed time

A path trace disproved the initial skipped-segment hypothesis: every channel
segment was visited, but in boarding state 1, which does not dispatch the
normal course events. The target advances by `elapsed_frames * 0.648148`,
while `82370588` emits a fixed `1.22685182` units of rider movement per update.
At two frames per update the target moves 1.296296 units, so the rider cannot
catch it. It follows the whole path in the wrong state, catches the endpoint,
and sees exit event 9 without the preceding heading-save event 10. This also
explains the zero heading that corrupted rotation and audio.

`race_boarding_hooks.cpp` corrects only the emitted boarding displacement,
after the original direction blend. It preserves the unit direction cache
and the exact one-frame behavior. If the target lies within this update's
travel budget, it performs the original paired-rider snap and pending-state
transition; the original caller still executes state exit/entry actions.
It does not alter global constants, replay unrelated event handlers, or
modify other riding states.

The regression reproduced the moving-target failure before the correction
and now covers fractional, two/three/eight-frame updates, both rider slots,
bounded target capture and unchanged excluded states. The corrected forced-two-frame desktop replay reached lap 2 at present 18000
and the post-race Replay menu by present 24000 (209.266 seconds wall time).
Its 206.624 seconds of raw guest audio contain zero nonfinite samples in all
six channels, with no invalid rider reports. The trace now records heading-save
event 10 before exit event 9. The normal Android APK also finished Frozen Forest and reached Replay at
present 27000. Its 761.611 seconds of complete raw audio frames contain zero
nonfinite samples in every channel (4096 trailing bytes from shutdown were
excluded). At present 17400, the HUD shows lap 3, time 03:18.15 and speed 156.

### Forgotten Tomb: one rider's exit resets other riders' launch timers

The normal Android run reaches the first ramp section, then flies above/outside
the course at roughly 01:20 and remains on lap 1 through the test limit. An
earlier fixed-two-frame desktop reproduction shows the same behavior; one-frame
Windows coverage finishes. Both captures have the same intentional upward
launch acceleration, approximately 0.305 units per update squared. The one-frame
control leaves physics submode 3 after roughly 70 updates; the failing two-frame
capture continues accelerating until reset. Incorrect gravity sign is therefore
not supported by the control comparison.

A later matched two-frame desktop baseline demonstrates a separate, precisely
traced failure in the same launch owner. At present 27900, its HUD remains on
lap 2 of 3 at 08:15.28, speed zero, with the rider against a wall. Owner
`8238E7B8` updates twelve 224-byte rider records. State 6 waits on the shared
owner countdown at `+3824`; state 7 launches until the countdown at `+3832`
reaches zero. Each countdown decrements after the owner's record callbacks.

Exit callback `8238D108` resets those shared countdowns to 100 and 70 on every
call, before checking the exiting rider's landing flag (`+2208 & 8`). Until
that flag appears, it retains the state-0 exit record and repeats the reset
next update. In `forgotten-launch-baseline-long`, retained record 3 repeatedly
writes wait `99 -> 100` while human record 0 and other peers remain in state 6;
the owner then decrements to 99 again. At present 24280 the same owner has ten
waiting peers and this retained exit record. The pattern continues through
present 27999. This is a confirmed countdown starvation, not a stopped renderer
or an inference from elapsed HUD time.

The initial `race_launch_hooks.cpp` guard wraps only that exit callback. It snapshots the wait
countdown when another existing record is in state 6, and the launch countdown
when another is in state 7. All original exit actions execute, after which a
countdown is restored only if its same rider, owner and state still exist.
The correction does not preserve phase bytes or alter clocks, positions,
landing flags, animations or record removal. Without an existing waiting or
launching peer, the original resets remain unchanged; newly admitted approach
records do not inherit an old cohort's timer.

The regression first failed with the original repeated reset, then passed with
the guard. It checks waiting and launching peers, both ends of the record list,
unchanged cleanup side effects, independent timer ownership, countdown zero,
and removed/reused/advanced peers. A standalone hook compilation also passed.
The matched private fixed-two-frame run reached post-race Replay by present
27900, where the baseline is still stalled on lap 2. Its trace exercises the
guard: retained exit records 1 and 2 coexist with waiting peers while the wait
countdown decreases from 97 to 87 to 77, then continues to zero; the human
transitions from state 6 to 7 at present 15664 and to state 8 at 15784.

The original shared phase arbitration can still delay an active launch when a
later waiting record writes phase 1 over phase 2. A separate reset also extends
the fixed run's roughly 119-update launch: `8238C868` logs launch countdown
`24 -> 70` at present 15711 and `67 -> 70` at 15714 while human record 0 remains
in state 7. The next launch callback sees the reset value, and retained exit
records 1 and 2 have disappeared by 15720. This delay therefore cannot be
attributed only to phase arbitration. Another launch has a roughly 100-update
countdown pause before completion. Both delays are bounded in this passing run.

The owner's mode-5 removal path calls `8238CFF8` for one record, then resets the
owner through `8238C868` at caller LR `8238E9DC`, outside the guarded exit
callback. That reset leaves other riders' launch states and accumulated travel
intact. Existing logs do not capture this caller address, so a bounded private
probe is prepared to identify this exact path with a surviving human state-7
peer. Constructor and whole-cohort resets must remain distinguishable. The
current correction leaves phase arbitration and this reset path unchanged.
Neither these bounded delays nor the matched forced-step completion establish
the cause or resolution of the earlier elevated stall. Prioritizing active
launches would also change waiting-cohort scheduling and needs separate evidence.

The normal Android run with the countdown guard still reproduces a persistent
elevated stall. In `forgotten-corrected-baseline`, screenshots at presents
15000, 17400 and 21000 show the rider above the same roof on lap 1, at HUD
01:34.11, 02:58.35 and 05:06.89, all at speed 58. The similar roof scale does
not establish unbounded upward acceleration; it establishes several minutes
without course progress. This remains unresolved by the countdown correction.

A subsequent normal-clock Android run with owner logging and full rider-state
capture does not reproduce that stall. `forgotten-launch-fixed-trace` reaches
lap 2 at present 17400 (03:02.53) and lap 3 at 21000 (05:18.15). Its 900 rider
samples span presents 12000–20990. Both upward launch callbacks complete:
the first moves state 7 to 8 at 15076, and the second at 17983, each after 70
owner updates. Sampled height peaks at about 618, then returns to the course;
the final sample is grounded in physics mode 7. The capture also shows approach
falls and a later respawn, so this is progress evidence, not a clean full-course
pass. Logging and full-state capture can perturb cohort timing. The failed
baseline has no equivalent state trace, so neither the exact elevated-stall
state nor a further production correction is established. A future reproduction
should capture only human position/mode and its launch owner state/countdowns
to reduce diagnostic overhead.

A smaller normal-clock Android diagnostic, `forgotten-launch-minimal`, reaches
post-race Replay at present 22800, verified in the full screenshot. It reuses
the normal production objects, including the existing countdown guard, and
adds only an owner hook logging the human's state changes and a sample every
60 presents. It has no forced timestep or full rider dump. The three human
launches enter state 7 at 14665, 17287 and 20032, reach state 8 at 14737, 17358
and 20103, and run the finish callback on the following update. Their exit
records clear at 14832, 17459 and 20133. The third approach enters physics mode
9 below the course but still completes its launch and returns to mode 7.

The same-APK, same-settings repeat, `forgotten-launch-minimal-repeat`, also
reaches Replay at present 22800, independently verified in its full screenshot.
Its three human launches enter state 7 at 14847, 17729 and 19527, reach state 8
at 14918, 17800 and 19598, and run the finish callback on the following update.
Their exit records clear at 15008, 17839 and 19638. Both later approaches use
physics mode 9 but complete their launch and return to mode 7.

Both runs were intentionally stopped after verified Replay menus. Their
unmet present-limit assertions are documented by the respective
`forgotten-launch-minimal[-repeat]-intentional-stop.json` markers and are not
counted as crashes. These two instrumented passes do not reproduce or explain
the earlier elevated stall. The first trace also shows a waiting countdown
increase after the human has exited; the original owner's mode-5 respawn path
can call `8238C868` and reset shared timers outside the guarded exit callback,
but this coarse sample does not identify the writer or establish another
causal bug. No additional production correction follows from these runs.

The final ordinary Android preview, `forgotten-normal-preview`, also reaches
post-race Replay at present 22800. This APK has no temporary launch-owner or
rider-state hooks, no forced timestep, and no local-function-binding trial.
Its SHA-256 is
`839d31b2cd647e09a91851f1236d9c3c2f8000bbd191ca0e4c853a0144562141`.
Screenshots confirm lap 2 at present 17400 (03:04.12, speed 183), lap 3 at
21000 (05:23.99, speed 188), and the Replay menu at 22800. The white/above-course
image at 15000 is transitional in this run, unlike the earlier baseline that
remained over the same roof for several minutes. This establishes one ordinary
preview completion; it does not explain the earlier intermittent failure or
justify declaring all Forgotten Tomb timing cases fixed.
The runner subsequently reached its 28000-present limit normally (`STOP
present-limit`), pulled 68 screenshots and exited successfully. Cleanup verified
that the game process had stopped and `debug.env` was absent; the ordinary
preview remains installed on Thor.

Ignored evidence: `out/issue-31/forgotten-motion-step1/`,
`forgotten-motion-step2/`, `forgotten-launch-step2-pinned/`,
`forgotten-launch-step3/`, `forgotten-launch-baseline-long/` and
`forgotten-launch-fixed-long/`. These forced-step tests establish functional
behavior under the stated timestep, not realtime performance.

## Coverage recorded so far

Windows functional coverage has now reached results for all eight Standard
and all eight Expert layouts. The runs use documented binary snapshots and
fixed-one-frame timing; the last two Standard courses and all Expert layouts
use the private menu fixture described below. This does not certify all
characters, alternate paths, rendering quality, or low-FPS behavior.

| Device | Mode / course | Evidence | Limitation |
|---|---|---|---|
| Thor | Dolphin Resort Standard | Reached post-race Replay menu | Pre-fix APK; not all paths/gears |
| Thor | Rocky Ridge Standard | Reached post-race Replay menu | Pre-fix APK; not all paths/gears |
| Thor | Metropolis Speedway Standard | Lap 3 and post-race Replay menu | Pre-fix APK; not all paths/gears |
| Thor | Frozen Forest Standard | Corrected normal APK reached lap 3 and post-race Replay; 761.611 seconds of complete audio frames all finite | One route/rider; physical sound quality still needs a listening check |
| Thor | Forgotten Tomb Standard | Final ordinary preview and two minimal normal-clock diagnostics reached post-race Replay at present 22800 | The later phase guard also completed a normal Thor run; approach/respawn motion and wider timing coverage remain open |
| Thor | Magma Rift Standard | Pre-fix Loading crash; corrected APK reached post-race Replay | One corrected run; not proof of all timing interleavings |
| Thor | Final Factory Standard | Private menu fixture selected Final Factory; post-race Replay at present 27000 | Normal elapsed-time simulation; one route/rider; not unlock-progression coverage |
| Thor | Metal City Standard | Private menu fixture selected Metal City; post-race Replay at present 27000 | Normal elapsed-time simulation; one route/rider; not unlock-progression coverage |
| Thor | World Grand Prix mission 2 | Tails Ring Grab Challenge entered, failure/Retry and a second attempt in the same process | Did not collect 100 rings; no mission 3 or full GP completion |
| Ally | Frozen Forest Standard | Passed ice channel in baseline; longer run reached lap-1 channel at speed 374 | Device became unreachable; longer run's finish/exit unknown |
| Ally | World Grand Prix | Finished first Speed Showdown mission and entered Tails ring-collection mission in same process | Second mission/full GP completion not established |
| Desktop | Dolphin / Rocky / Frozen / Metropolis / Magma / Forgotten Standard | Reached results and Replay with lifetime/zero-heading fixes | Fixed one-frame accelerated functional coverage |
| Desktop | Frozen Forest, forced two-frame diagnostic | Vulkan and confirmed D3D12 both reach post-race Replay; Vulkan raw audio all finite | Accelerated reproduction, not a realtime benchmark |
| Desktop | Final Factory / Metal City Standard | Both reached post-race Replay | Private menu fixture; fixed one-frame functional coverage |
| Desktop | Final Factory Expert | Finished in 06:51.81; results and saving screen captured | Private menu fixture; present limit before Replay capture |
| Desktop | Dolphin / Rocky / Frozen / Metropolis / Magma / Forgotten / Metal City Expert | Finished and reached post-race Replay | Private menu fixture; fixed one-frame functional coverage; Dolphin black road regions are a separate rendering observation |
| Desktop | World Grand Prix missions 1 and 2 | Mission 1 failed, retried and succeeded in one process; six deliberate mission 2 input-script attempts reached normal failure/Retry | Best observed mission 2 attempt had 72/100 rings; mission 3 was not entered; no full GP completion |

The original isolated save exposes six Standard courses. A private opt-in
menu fixture now exposes Final Factory, Metal City and Expert layouts by
overriding only the original temporary availability callback on Free Race
pages 47/48/55. It does not write profile/save unlock flags. The first attempt
targeted a different menu and remained on Dolphin Resort; that run is explicitly
excluded from Final Factory coverage. The corrected fixture was verified by
its activation log and the visible Final Factory Standard selection screen.
Remaining layout tests use this private binary and must not be confused with
unmodified menu/unlock progression tests.

## Reference comparisons

Local UnleashedRecomp `5e8695a` and MarathonRecomp `b4d5639` use targeted timestep
and damping corrections; their solutions do not justify multiplying every
Free Riders clock consumer indiscriminately. Xenia's floating-point and audio
implementations were also inspected. In this reproduction, the first invalid
angle has a zero vector input, so replacing multiply-add semantics or changing
the host audio queue would not repair that operation.

References: [Xenia PPC floating point](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/ppc/ppc_emit_fpu.cc),
[Xenia x64 operations](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/backend/x64/x64_sequences.cc),
[Xenia audio system](https://github.com/xenia-project/xenia/blob/master/src/xenia/apu/audio_system.cc).


## Renderer: broad translucent bands

An isolated Windows Dolphin Resort Expert diagnostic attributes the broad gray
bands to a specific unsupported texture, rather than to the host touch overlay
or input tracing. Framebuffer screenshots are taken before the presentation
blit draws host controls; input/menu trace options only log state. This test used
1280x720 at native internal resolution, fixed-one-frame simulation, no audio,
`SFR_MAIN_AFFINITY=0`, a private course-selection fixture, and a copied save.
It is functional evidence, not a realtime benchmark or an ultrawide comparison.

At present 14400 in `out/issue-31/band-detail-dolphin-14400/`, draw 598 adds
the first broad band: compare `out/before-0598.bmp` with `out/after-0598.bmp`.
Draws 598–602 add the remaining bands (`out/after-0602.bmp`). Each draws ten
vertices using pixel shader `0x40293640`, caller `0x8280f2a8`, and the same
texture fetch: format 15 (`k_4_4_4_4`), physical address `0x0f134000`, 256x32,
linear, pitch 256 texels, endian 1, swizzle `0x60a`. The renderer previously
substituted an opaque white texture for this unsupported format. Of the actual
8,192 captured texels, 6,200 have zero alpha and only 22 have full alpha, so that
fallback exposes otherwise transparent ribbon geometry.

The targeted fix expands the four nibbles to eight-bit components after endian
conversion, preserving the texture's alpha and XYZW/ZYXW view. Support is limited
to linear unsigned normalized format-15 textures with endian 0 or 1 and the two
audited swizzles; tiled, signed, integer, exponent-adjusted and other swizzle
variants remain unsupported. The channel mapping was checked against Xenia
commit `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`:
[packed component conversion](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/shaders/pixel_formats.xesli#L388),
[16-bit texture loading](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/shaders/texture_load_16bpb.xesli),
and [byte-pair endian conversion](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/shaders/endian.xesli).

The native-format regression first failed because this fetch had no supported
layout, then passed with tests for captured texels, distinct channels, transparent
alpha, padded rows, truncated input and unsupported-mode guards. The isolated
fixed replay in `out/issue-31/band-fixed-dolphin-14400/` binds the decoded texture
in draws 518–522. Comparing `out/before-0518.bmp` with `out/after-0522.bmp` shows
fine translucent streaks instead of broad flat bands; `frame-14400.bmp` shows the
same improvement in the completed frame. Both runs captured identical guest
texture bytes (SHA-256
`3e784e5dbaf8bad73d5f444fd9f4da33ce1c383a9580488c2a88e4d07ebf4a04`).
Asynchronous loading changes race position between runs, so these are per-draw
causal comparisons, not matching-frame pixel comparisons. Both bounded runs
reached their present limit without timeout and left the source save unchanged.

Black road regions are already present before these five draws and remain after
the format-15 fix; the separate cause is recorded below. Format 12 was captured separately
at 128x128 and 32x32 (both with a 128-texel pitch) and remains unsupported; no
format-12 color conversion or road fix is claimed. The original reporter's
brighter ultrawide corners have not been reproduced at that aspect ratio, so this
band fix must not be treated as proof that all reported corner brightness is
resolved.

## Renderer: black Dolphin Resort road

Per-draw captures establish that the road material renders first, then a later
multiplicative pass turns it black. In the isolated fixed-one-frame Expert case
`out/issue-31/road-dolphin-13200/`, compare `out/road-0496-before.bmp` with
`out/road-0496-after.bmp`: draw 496 covers the textured road while leaving the
riders and surrounding walls visible. It draws 34 vertices with vertex shader
`0x404e6ed0`, pixel shader `0x404e7d40`, caller `0x82809528`, and target
`0x71700000`. Its RGB blend is source times destination color, with no separate
destination contribution. The sampled RGB-black pixel count increases from 0
to 99,194 (one sample per four pixels).

The captured host shaders matched entries in the packaged shader cache; their
guest source bytes then matched the retained translated shader sources
`v9-e41a6770c8eecf2e-1880` and `v9-0eb1cb4daec08ad6-928`. The pixel shader
samples a base texture and a depth-shadow texture. The latter's depth resolve
is unimplemented and its captured guest bytes are zero, but this alone is not
the demonstrated cause: the shader limits shadow darkening with a nonzero
shadow color. Captured positions, constants, half-float texture coordinates,
and evaluated vertex projections are finite.

The actual defect is the base texture's packed layout. Its fetch words are
`80400002 10f13086 0001e00f 00a00c14 003b0042 00000a00`: a tiled 16x16
format-6 texture, pitch 32, endian 2, ZYXW swizzle, with packed mips enabled.
The decoder read the packed flag from word 5 bit 0 (a border-color bit), instead
of bit 11, and base-level extraction did not account for the packed origin.
Xenia commit `95a5c3ee250f80c3b9d139658649d9ffb6db3eec` defines
[the flag in bit 11](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/xenos.h#L1281)
and [the packed mip offset](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/texture_util.cc#L116).
For this texture, the base image begins at texel (16,0). In
`out/issue-31/road-detail-dolphin-13200/`, the captured tile's wrongly selected
16x16 region has 192 transparent-black and 64 white texels; the correct region
has 256 opaque-white texels. Sampling the black region during a multiplicative
draw makes the road black, even though its underlying material was drawn.

The fix reads the correct flag and extracts the packed base at its block-space
origin, once, before texture upload or decompression. Focused tests first failed
for both the flag and the white 16x16 base region, then passed. Coverage includes
rectangular textures, dimensions sharing the same rounded-up log2 size, linear
and tiled storage, BC block units, row pitch and missing input. Lower mip-chain
loading remains outside this change.

The isolated replay `out/issue-31/road-fixed-dolphin-13200/` captures the same
34-vertex shader/fetch combination at draw 777. Its before/after RGB-black
counts are both zero, and `out/road-0777-before.bmp`,
`out/road-0777-after.bmp`, and `frame-13200.bmp` retain the textured road.
The baseline and fixed runs captured identical raw base-texture bytes. Race
positions differ because of asynchronous loading, so these are causal per-draw
checks rather than matching-frame image comparisons. Both probes reached their
bounded present limit without timeout and left the original save unchanged.
This demonstrates the captured black-road repair, not accurate depth shadows,
all-course visual correctness, or resolution of the reporter's ultrawide corners.

## Wider output-window check

Two bounded Windows Vulkan cases used the same normal binary (SHA-256
`92aec2086dc683e4ead4f653a2f280ae1248baeccb6bd7755c6e14ea6516e1e4`),
Dolphin Resort Standard route and independent saves. Startup logs verify actual
swap chains of 1280x720 and 1680x720, with 1280x720 internal rendering in both.
Each reached its 18002-present limit, produced six internal screenshots, and
preserved the source save. Sampled gameplay frames show no obvious additional
corner brightening in the wider-output case, but race positions differ.

These captures are taken before final presentation. The attempted live-window
capture tool did not return, so the final displayed image and side bars were
not visually verified. Source inspection predicts a centered 1280x720 image
with 200-pixel black side bars in the 1680x720 output; this is not an expanded
field-of-view test. No rendering change is justified by this comparison.
Exact settings, hashes and limitations are in ignored
`out/issue-31/aspect-comparison/comparison.json`.

## Reporter-specific limits

Issue 31 also reports brighter widescreen corners and left-side ring reach.
The pending controller hook changes address the ring-reach input path, but
physical controller feel still needs user confirmation. The corner brightness
has not been reproduced or attributed by this investigation. The reporter has
not supplied a GPU/backend, crash log or the two specific Grand Prix missions.
Loading race reproduction on both local platforms provides a concrete plausible
crash cause; it does not establish that all reporter crashes are identical.

## Additional October 2 follow-up evidence

The remaining six Standard courses passed the private two-frame simulation
fixture: Dolphin Resort, Magma Rift, Rocky Ridge, Metropolis Speedway,
Final Factory and Metal City all reached post-race Replay. Selection and Replay
screenshots were inspected; each case preserved the original save. The private
executable SHA-256 is `513d993a3bf8d50cdd101ba815c27b007f5654e5404f3427aa98f12ae23cd953`.
Together with the earlier Frozen Forest and Forgotten Tomb two-frame cases,
this broadens functional timing coverage to all eight Standard courses, across
the documented test builds. These unpaced, forced-step runs are not benchmarks.
Per-case evidence is in ignored `out/issue-31/desktop-fixed2-coverage/coverage.json`.

Thor's normal-clock `forgotten-peer-reset` nevertheless reproduced the prolonged
lap-1 elevated stall, including screenshots at 17400 and 27000. The narrowly
filtered mode-5 peer-removal probe observed no qualifying reset before that
failure. A later delayed trace, `forgotten-launch-late-retry`, supplies more
specific evidence: the human enters launch state 7 at 17880; at 17940 and 18000
its launch counter stays at 38 while the owner's final phase is 1 and later
records 5/9 wait in state 6. Meanwhile its Y position grows from 480.656 to
2136.8. At 18052 it finally reaches state 8; subsequent exit movement remains
outside the course, with Y above 1.9 million by 21480. The original state-6
callback `8238DFB0` writes phase 1; state 7 writes phase 2, and the owner only
decrements the launch counter if phase 2 survives all twelve callbacks. This
is direct evidence of a second shared-state timing conflict, beyond the
already corrected repeated exit timer reset.

The phase-cohort prototype completed Windows fixed-one-frame and forced-two-frame
cases, and a normal-clock Thor run reached Replay at present 27000. Thor's five
human launches observed after the delayed trace began lasted 70–71 updates;
some approaches still fell below the course and respawned, so this is completion
and countdown evidence, not proof of correct motion on every approach. The
prototype APK hash is `8c28000e1da6e1f272f6d47fc1e06c4b9611c6d34915814ce95466078df05e99`.
The Windows prototype hash is
`29612ee512970fb19b22adcfad6a56d008ec82d79540d39315b964c8220c4691`.

The scoped guard is now integrated: waiting (`8238DFB0`) and exit (`8238E620`)
callbacks keep all original effects, then restore launch phase 2 only for an
already active peer that still has the same rider, owner and state. The two
single-peer reset call sites (`8238E9DC`, `8238D36C`) additionally preserve that
surviving cohort's launch countdown. Constructor and whole-cohort reset callers
are unchanged. Unit tests first reproduced the paused countdown, then passed
with the guard; they also cover both record orders, removal/reuse/advancement,
new admissions, sole riders, original cleanup, and nested landing resets.
A normal Windows binary without owner tracing reached Replay by present 42000
(SHA-256 `a0b1f9f4b527b2d5d65d9a282f962ba48379b226ac26aaf2a9177a3415585246`).
The equivalent normal Thor APK (`bda92c8895977a22ea40e5e1a9278be61d98dc22c26ae36674784efc7295dd3e`)
also reached Replay, verified at present 22800. The owned game process was
intentionally stopped after that screenshot; the runner therefore lacks its
normal present-limit marker. The stop is recorded separately and is not a
crash or a full-limit pass.

The first delayed-trace attempt stopped before race entry with
`worker-memory-access @0x59000100`, from `82A6FCE0` on the audio worker's
`82727830` path. This routine dereferences `KeDebugMonitorData`; `0x59000100`
is the raw import marker beneath the provider, not the mapped null-monitor
cell (`0x72400000`). Runtime thread creation registers guarded process fields;
the previous whole-memory fast-page rebuild temporarily published plain
readable permissions for existing provider pages before restoring guards.
A concurrent regression reproduced a provider page becoming directly readable
during unrelated registration. It failed before the correction and passes
afterward. Registration now rebuilds only its affected page, permissions are
composed privately and published once, flag accesses are atomic, and concurrent
special-word scans hold the layout read lock. Windows and Android full game builds succeed. All 72 test targets directly
compiling GuestMemory were rebuilt, and the complete 140-test Windows CTest
suite passes. The Frozen Forest and normal Forgotten Tomb completions below
also exercise this fix. This newly captured startup
failure is separate from the loading actor-lifetime and Frozen Forest fixes.

The normal memory-fix Windows binary also finished Frozen Forest and reached
Replay at present 31800 (SHA-256
`22637bc1d333be7dc0484e5a67f3e82ff1e48d55cac5cd76982c176971163e1a`). Its 244.539
seconds of complete raw audio contain 70,427,136 finite samples across all six
channels, with no trailing partial frame. Peak source magnitude is 2.130;
finiteness does not certify clipping behavior or audible quality.

The first memory-lock implementation inlined its shared-lock machinery into
generated loads, growing the Windows executable from 133,083,648 to 170,552,320
bytes. Moving the guarded-word scan out of line retains identical locking
and guard semantics, and the rebuilt Windows executable is 121,136,640 bytes
(SHA-256 `8b5e56165aebe3721a40a4bcd875501fbe681fc4d623b455c5ad05abdfc7e7eb`).
Both platform builds succeed and all 140 Windows tests pass again (36.29 s).
The ordinary Android APK is `19588fe58710f4739b8ba6de071c55c9fabe19a1296f6a4a42809fa6002802a8`;
its unstripped native library is `ded1ef5e00343d6528a817654693328d324ce8f0e0b39a5cafc94c34031dc738`.
The compact Windows binary selected Forgotten Tomb Standard (present 8400) and
reached Replay at 42000 in `desktop-compact-stability/forgotten-compact-explicit`.
Its bounded run exited normally after 309.422 seconds and preserved the source
save. An earlier run using the unsupported short speech token `forgotten`
actually selected Dolphin Resort; it is explicitly excluded from Forgotten
Tomb coverage. No speedup is inferred from binary size or these functional runs.
Thor Expert coverage of the compact objects is now running through the private
menu fixture. The separate main-core placement request was not applied, so
that trial supplies no performance comparison.


### Compact Android Expert coverage

The private course-menu fixture linked against the compact production objects
uses normal elapsed-time simulation, audio and 720p rendering. APK SHA-256:
`a539c816b9180c04311b620b540f443289837898b488dca4509f04b3520b0416`.
Dolphin Resort Expert was visibly selected at present 8400 and reached Replay
at 19200. Two matching Replay labels trigger an intentional owned-process stop;
the captured image was then visually verified. This is a completed race, not
a present-limit exit. Remaining Expert routes are still being tested.
Selection/results and per-case limitations are tracked under ignored
`out/thor-race-perf/expert-compact-verification.json`.


The compact normal Windows binary also completed Frozen Forest Standard with
confirmed D3D12 on the RTX 4090, reaching Replay at present 36000. The case
`desktop-compact-stability/frozen-compact-d3d12-runtime` exited at its bound in
265.750 seconds and preserved the source save. The earlier similarly named
case lacked the Agility runtime beside its private executable and automatically
fell back to Vulkan; it is explicitly excluded from D3D12 coverage.

A further Grand Prix stress test is investigating session thread-slot lifetime:
`GuestThreads::create` monotonically consumes one of 96 slots, while closing
an exited thread does not reclaim its slot. Existing mission transitions create
new threads, but no observed Issue 31 crash has yet been attributed to this
limit. The new same-process mission-2 Retry run uses a copied legitimately
completed mission-1 save; retries are not completion of later GP missions.


### Completed thread slots in long sessions

A standalone regression reproduced `thread-capacity` after 96 sequential workers
had already completed and had their guest handles closed. `GuestThreads` never
reclaimed the finite guest-address slots. The correction reuses only an equal
stack extent after native completion/join, handle closure and zero explicit
object references. It resets PCR, object, stack and TLS contents while preserving
the guarded process-information word. Old records and native exit handles remain
alive for borrowed waiters; guest handles and IDs are never recycled. Historical
records no longer participate in object lookup after their storage is reassigned.

A first full-game prototype caught a missing integration detail: the TLS registry
rejected the reused bank address. That prototype stopped during startup and is
excluded from delivery. The corrected factory receives `reused_storage` and keeps
the already registered address bank, whose values were reset during initialization.
The 128-worker regression now includes the TLS registry, exclusions for open
handles/references/running workers/different stack extents, and retained exit-handle
waits. Windows' complete 140-test suite passes in 27.42 seconds; the native Android
regression also passes on Thor. Both platform games rebuild. Fresh race validation
is still required before replacing the delivery snapshots.

The separate same-process GP mission-2 Retry stress reached its 90000-present
bound in 836.234 seconds without a crash, with maximum created guest ID 41. It
therefore did not reach this capacity defect. Repeating one mission is not the
reporter's sequence of five different GP missions, and this correction must not
be presented as proof of the reported GP crash's cause.

The corrected thread/TLS Windows binary (SHA-256
`a70082189b3d91578a22c83701781a61242a6e3d9bf3874b65b09ef19a5a56ba`)
completed Frozen Forest Standard: selection 8400, lap 3 at 24000 and Replay
at 36000, all visually verified. The 42000-present bound ended normally in
308.078 seconds. This fixed-one-frame Vulkan functional check preserved the
source save and is not an FPS measurement. A same-process multi-course route
is being checked separately.

Independent review additionally found that monotonically allocated handles must
stay below the existing thread dispatch limit, even after storage reuse removes
the earlier 96-creation ceiling. Creation now rejects handle exhaustion before
any storage or output mutation. A boundary fixture seeded only the initial
identity at the last valid handle; it failed before the guard and passes afterward.
A tracked regression also verifies a throwing factory after storage reuse leaves
the partly initialized slot retired, preserves old exit handles, and permits a
later allocation elsewhere. Final full Windows suite: 140/140 in 29.20 seconds;
final Thor native tests pass. No Issue 31 reproduction has been tied to the
handle namespace boundary.

### Metropolis Speedway Expert: reported uphill-curve falls

The user observed repeated off-course falls on Thor at the boost-pad section
that curves upward, even though the race eventually finished. The current batch
was Metropolis Speedway Expert; its Replay at present 24600 confirms completion
only, not normal traversal. Screenshots at 14400 and 18000 show the vertical
shaft area; matched current-code fixed-one/two-frame diagnostics are being
prepared to distinguish neutral-input limitations from timestep/physics faults.
This supersedes any interpretation of a Replay screen as full-course correctness.
Those initial captures did not establish a correction. The scripted-motion
section below records the subsequent isolated cause and fix.

The current-code private probe (`b968197b9e9875b8ff4988a4ee0d21acd5416eff625f159c5ac6e7036716452f`)
reproduces a timestep-dependent difference with neutral input. Recorded clock
steps are exactly one or two after present 13200. The one-frame run exits the
uphill sequence and continues around the course; the two-frame run repeatedly
returns to the same uphill section. Positions stay finite. This narrows the
investigation to course traversal/physics under larger steps; it does not yet
identify the incorrect calculation. The runs enter the race at different present
counts, so comparisons must align by world position/state, not screenshot number.

### Final Factory Expert: live Thor tube stall

The user reported the next Thor course was stuck. Selection screenshot 8400
confirms Final Factory Expert. At present 18000 the rider is horizontal in the
blue tube, lap 1, speed zero, race time 03:49.65. A live ADB screenshot at 10:31
shows the same section at 10:47.61, still lap 1 and speed zero. Rendering and the
timer continue. The bounded run ended at 30000 with `STOP present-limit`, without
finishing the race. This is a failed traversal check, not an application hang or
a successful coverage result. The log and screenshots are preserved under
`out/thor-race-perf/finalfactory-expert-compact*`. Subsequent matched one/two-frame
Windows diagnostics isolate a distinct cause, recorded below.

### Same-process course transitions after thread retirement

The TLS-corrected Windows snapshot `a70082189b3d91578a22c83701781a61242a6e3d9bf3874b65b09ef19a5a56ba`
reached its 204000-present bound without timeout in 2378.609 seconds. Visual
evidence confirms Frozen, Dolphin, Forgotten and Magma Standard results. The
fifth planned Rocky selection arrived after the menu had already advanced, so
the last race repeats Magma and cannot count as Rocky coverage. Guest IDs reach
83 while 33 distinct PCR addresses serve the traced workers, with 47 observed
reuses. This validates real course transitions with recycled storage, but is
neither five different GP missions nor proof of Issue 31's reported cause. The
runner verified that the source save was unchanged. Evidence and limits are in
`out/issue-31/desktop-thread-retirement/five-course-session/verified-result.json`.

### Scripted course motion: two independent timestep defects

The investigations above now have isolated causes, reproduced with the same
neutral-input Windows fixture at normalized steps 1 and 2:

- Final Factory's blue tube uses physics submode 1 in `822AB3B0`. It adds the
  supplied external displacement once per update while gravity uses elapsed
  frames. At step 2 the launch peaks around y=399 instead of reaching y=483 and
  the transfer trigger. Scaling only the supplied xyz displacement by the
  current frame step restores the original transfer; the force-only prototype
  passes the tube at step 2 and continues on the course.
- Metropolis's uphill path in `8231CA00` tests the previous node for equality
  with its phase boundary. Route 1 can jump from node 143 to 149, skipping 144,
  and remain in deceleration until path exhaustion. The path-only prototype
  repairs the missed phase transition and traverses that section twice at
  step 2, restoring the original surface-hit exit near node 187 instead of
  exhausting the path at 190 and falling back down.

These prototypes were tested independently. Their respective hashes are
`52adedb77a30cf2629cac365ee474d0e8f29c70e9874edddd7f5f3b0a65ada30`
and `4e268e7cac30076f77da4925c419b31f76345709736dfab319d39865d8b3033e`.
The latter directory is called `win-course-combined-probe`, but its measured
build deliberately omits the force prototype. Forced-step runs are functional
checks, not FPS measurements.

Production hooks now make both narrow corrections. Supplied motion is restored
after the original call without undoing intentional handler changes; invalid
inputs and overflowing products are left untouched. Path repair is limited to
the two original route records, a still-owned path that has already passed its
boundary, and an unchanged owner/route without a node reset. Original handlers
still run once and global time is unchanged.

The new regression failed against invoke-only helpers for both missed elapsed
movement and missed boundary transition, then passed with the fixes. The full
Windows suite passes 141/141 in 13.12 seconds; the ARM64 motion regression also
passes on Thor. Independent review found no substantive issue. Current combined
production builds are undergoing actual Windows/Thor course validation.

The prior ordinary thread-retirement build also completed Frozen Forest Standard
on Thor (Replay at 20400) and on Windows D3D12 (Replay at 39600). The Thor audio
capture has zero non-finite samples across 157,518,848 samples; this establishes
numeric validity, not subjective sound quality. These results predate the new
scripted-motion fixes and are not substituted for their device validation.


The combined Windows fixture now passes the previously failing step-2 cases and
larger step-3 cases on both courses. Its SHA-256 is
`c4846f6a81a881cd96cfc2a219a7893c256f1cc2f9661599b50153accb74087f`.
It substitutes only the diagnostic clock/observer/menu fixture and retains the
new production motion hook object. Position samples remain finite. Final Factory
transfer events appear at 15990 (step 2), 15245 and 18235 (step 3); Metropolis
is on lap 2 beyond the uphill section at 18000 (step 2) and 17400 (step 3).
Results, screenshots, source-save preservation and limits are recorded in
`out/issue-31/city-motion/production-motion-verification.json`.

Thor's normal-elapsed-time course fixture
`fc4502d8be46d92ad3a49ea45a7a70b4c588543be56adc5488e78edd7771a2ec`
now completes Final Factory Expert. Selection at 8400, lap 2 at 16200 and
Replay at 22800 were visually verified. The first lap is 02:32.17; the old
fixture remained on lap 1 at 10:47.61. This fixture only unlocks course selection
in a private save; it has no clock override or full rider dump. It is not the
ordinary delivery APK. Metropolis device verification is recorded separately.


The final six-case Windows matrix completed normally at its bounds: both courses
at normalized steps 1, 2 and 3, with finite sampled positions and the source saves
unchanged. Step-1 screenshots also retain the original traversal: Final Factory
on-course after the tube at 19800, Metropolis beyond the uphill at 18600.
This matrix changes the clock only inside a private fixture; it is not evidence
of a frame-rate increase on any machine.


Thor also completes Metropolis Speedway Expert with the same corrected fixture.
Visually verified evidence: selection 8400, uphill shaft 13800, lap 2 at 15000,
after the second uphill at 16800, lap 3 at 18000, after the third uphill at 19200,
and Replay at 21000. The first two laps are 02:00.74 and 01:59.60. The previously
reported repeated uphill falls were not observed in this neutral-input run.
This is sampled traversal evidence, not exhaustive rider/branch coverage.

After the two device runs, the ordinary APK
`70dc3ab01e46b545491bd5312f4615b7c76c41796c30a016ede754b0e3ae4088`
was successfully restored and `debug.env` was confirmed absent, with no running
game process. The ordinary Windows executable is
`516d553957a0bb54fc3277d30ee2ff7aced69305b05b5c9e058f8d6772d0125b`.
The local delivery is main-checkout `out/course-stability-20261002`, including
validation metadata and limitations. A fresh Ally SSH attempt still timed out;
this does not establish coverage on Ally or resolution of all Issue 31 symptoms.

## Post-v0.4.6 resolved-texture allocation lifetime

A separate renderer regression reproduces stale pixels after a guest physical
allocation is freed and reused. The existing physical-memory free hook calls
`NativeRenderer::invalidate`, but that method previously removed only ordinary
texture-cache entries. A resolved framebuffer at the freed address remained in
`resolved_targets`, and its lookup took precedence over new guest texture bytes.

Invalidation now removes resolved mappings whose destination base is inside the
invalidated allocation, clears their dimension tag, and advances the binding
generation. It queues their descriptors through the existing deferred recycling
mechanism; recorded and submitted draws retain their texture objects until GPU
completion permits reuse. This fixes whole-allocation lifetime handling. The
resolve's guest byte extent is still unknown, so partial writes excluding its
base and writes bypassing invalidation are not covered.

The real GPU regression in `native_resolution_test.cpp` failed before the fix on
RTX 4090 Vulkan and D3D12, then passed on both. It checks distinct old/new pixels,
recorded and asynchronously submitted consumers, a resolve inside a larger freed
allocation, repeated invalidation, an unrelated resolve, descriptor reuse, and
dimension tags at 50% and 200% rendering scale. The Android ARM64 renderer library
also builds. Raw red/green output and independent lifetime review are retained
under `out/continuation-20261002/`.

This demonstrates a resource-lifetime defect, not the cause of the reporter's
Grand Prix crashes or bright corners. Reporter logs/screenshots are still needed
to connect those symptoms to a specific fault. No new release is implied by this
investigation entry.

The corrected private Windows build subsequently completed Frozen Forest
Standard and Dolphin Resort Standard in one Vulkan process, with post-race
Replay screens at presents 34800 and 67800. It ended at its 80000-present bound
after 820.641 seconds with the source save unchanged. This used fixed-one-frame
functional timing, so it is neither a performance measurement nor Grand Prix
progression coverage.

The matching private Android build completed Dolphin Resort Standard under
normal elapsed-time simulation on AYN Thor, with Replay at present 21600 and a
bounded exit after 633.609 seconds. The source fixture remained unchanged. Its
instrumented 13200–17400 window averaged 46.274 ms per present, with a 91.106 ms
maximum. Course positions and draw counts differ from the earlier baseline;
this does not establish an improvement or regression. The exact ordinary
v0.4.6 Android 10+ APK was restored and checked by SHA-256, and `debug.env` was
removed. Visual verification, provenance, logs and cleanup records are in
`out/continuation-20261002/resolve-two-courses-vulkan` and
`out/continuation-20261002/thor-resolve-dolphin`.
