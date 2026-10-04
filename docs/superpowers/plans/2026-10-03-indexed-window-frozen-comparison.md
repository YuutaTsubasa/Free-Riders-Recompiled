# Guarded indexed-window comparison in Frozen Forest

Follow-up to the private bounded8x candidate measured in Dolphin Resort. The hypothesis is that reduced gather/repack work also lowers main-thread CPU cost in a different scene. Existing Dolphin comparisons are complete and must not be repeated automatically.

## Design

Use the frozen control35b701374f0f85f353f85e767c229bdc62bb6662d0b6723a047863c5b739a403 and candidate46d7a42a721180053b7abcc2745b22cbbd95986d47c7a72655b6c8a2c100788a. Both include the accepted ordinary-memory preflight; neither includes the later lifecycle correction8aa640d. This experiment isolates the same guarded indexed-window predicate, with no new compilation or production integration. Results cannot be attributed to a combined current-source build.

Run fresh Vulkan Frozen Forest cases on Ally X: directlog-index8frozen-base-ab, directlog-index8frozen-candidate-ac, directlog-index8frozen-base-ad. Only scene routing changes from the old Dolphin harness: dolphinresort@7800 becomes frozenforest@7800. Per-case independent saves and caches; normal game/UI timing, audio enabled, every necessary draw, brake-held stationary race. Use unchanged private120s race timing harness, middle100s presented-frame analysis and middle CPU samples. No shared cache reset; report driver-cache/prewarm differences separately.

## Steps

- [x] Prepare new ignored out/cpu-index8-frozen-v047 helpers, case manifests and analyzer with frozen inputs. Preserve old artifacts; validate analyzer selftest and matched environments offline.
- [x] Independent scope then quality review before device dispatch.
- [x] Fresh single preflight confirms idle Ally X and unchanged power/fixture. Run bounded control/candidate/control once, recording dispatch intent; stop on failure or offline state. Each case840s runtime/850s runner/16min task max, batch1500s. Temporary unique wake11 at most30min, finally release after owned task cleanup. No retries after ambiguous dispatch.
- [x] Verify raw CPU/frame and FPS/frame-time statistics, backend/hash, logical work metrics, screenshots and normal HUD clock. Discuss control drift and prewarm separately. No full-course, pixel, audio-quality, Android or statistical significance claim.
- [x] Release temporary wake, verify no owned games/tasks, source fixture and power unchanged, save evidence and continuation. Keep candidate private unless broader acceptance is separately justified. No push, merge, publication or external comment.

Ally X was reachable and idle at06:42:32UTC; this is not permission to skip a fresh preflight at dispatch. If unavailable, finish preparation and defer device work without reconnect loops. This test never contacts Thor or changes ordinary launch settings.

## Verified result

Fresh AB/AC/AD sequence completed once. All three processes exited3 at STOP benchmark-window and their owned scheduled tasks were removed. Independent final review recomputed raw CPU/QPC/TID, presented frame statistics, all20 metric means per case and42 input hashes, and inspected all six original screenshots. Full Frozen Forest starting scenery, rider and HUD were visible, with normal clock progression. No full-course or visual-equivalence result follows.

| Metric | First control AB | Candidate AC | Second control AD |
|---|---:|---:|---:|
| Main CPU ms/frame | 5.575853 | 5.536782 | 5.647116 |
| Process CPU ms/frame | 15.244983 | 15.319971 | 15.310035 |
| FPS | 107.169324 | 106.721312 | 106.676108 |
| P99 frame ms | 12.235096 | 12.372259 | 12.213334 |
| Maximum frame ms | 18.4113 | 42.9669 | 21.2447 |

Candidate main CPU/frame is0.70% lower than AB and1.95% lower than AD; controls themselves drift1.28%. FPS is0.42% lower than AB and0.04% higher than AD, within0.46% control drift. Whole-process CPU/frame increases slightly against both controls. This does not establish an overall Frozen Forest speedup or a stable tail regression. Candidate has the sole frame above25ms; one occurrence does not establish causation or statistical significance.

Gather time decreases from0.00832/0.00865ms to0.00501ms; repack bytes decrease from24,976 to4,368 per frame. These support reduced path work, but the absolute time saving is small in this scene. Draw and guest index counts remain similar, with normal animation differences; different byte/cache counts are path dependent and cannot be interpreted as dropped work. No pipelines are created in the measurement windows. Startup prewarm takes97.576/125.528/97.2798ms, reported separately; shared driver caches were not reset.

The candidate42.9669ms frame15142 reports32.6669ms main-ready time, with guest holder19 prominent. Thirteen successful NtSetInformationFile class14 position updates appear between the preceding frame and this frame. Pipeline, upload-wait and GPU-wait counters are zero for this frame; draw time is1.2651ms. This is a scheduling/holder-delay association, not proof that logging, I/O or the predicate caused it. Existing logs suffice for this observation; no additional trace was started during the comparison.

The first wake transport failed before loading the script because of the remote PowerShell execution policy. Its error is retained. A fresh read-only snapshot proved no wake metadata, stop marker or game existed; an independently reviewed, separately recorded one-time recovery used process-only ExecutionPolicy Bypass without changing permanent policy. Wake11 then ran with its original30-minute limit. After the batch, the explicit stop marker released it and its SSH transport exited0. Final snapshot07:17:58UTC confirms no games, owned tasks or wake process, unchanged Turbo power mode and unchanged source fixture files. No ordinary launcher settings or Android device were touched.

Artifacts: out/cpu-index8-frozen-v047 and new case directories under out/cpu-profile-v047. Final analysis SHA256101972ef52369b3d86b94dd306842a20691a1b0c559d021c96ec57583537f663. Parent verification SHA25611de4c2ecdf9ec7b50bf7b687ec31aca01eca5ef5ac861a9fc92520986ec9161 records all screenshot hashes and HUD/log intervals:45.39/45.3806s,45.77/45.7793s,45.60/45.5829s. Preparation READY and generic analyzer's pending-visual label remain historical; final verification supersedes them.

Disposition: keep guarded8x private. Dolphin's previous improvement cannot be generalized from this second scene, and the controls must not be pooled. Production remains4x with original worker2000; local accepted memory preflight/lifecycle correction remains separate. No runtime changes, push, merge, release or external comment. Do not repeat this completed stationary sequence without a new hypothesis. Useful next work is a targeted investigation of guest holder19 during the observed stall, or full-course/Android validation with separately identified artifacts and normal time.
