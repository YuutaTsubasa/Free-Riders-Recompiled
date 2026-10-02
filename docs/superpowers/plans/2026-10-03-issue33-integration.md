# Issue 33 sequential integration

User approved integrating the three contributions one at a time: benchmark,
checkpoint, then render thread. Preserve the contributor's authorship, keep
independent PRs, verify each before merging, and do not publish a release or
alter the user's running Thor test. Main checkout and camera-debug stay intact.
Use the managed issue33-integration worktree. Execution is serial except for
the independent review required by the requesting-code-review skill.

## 1. Benchmark harness

- [x] Import knuckleslee's 7095d21 and b24078a retaining author identity.
- [x] Run imported Python benchmark tests (40 pass).
- [x] Add regression coverage for isolated environment/cache/save handling,
      source preservation and safe output paths; correct proven defects.
- [x] Default to ordinary elapsed race/UI time, audio and complete rendering.
      Make diagnostic exclusions explicit and record effective settings/hashes.
- [x] Verify Python tests, PowerShell execution with a fake executable, native
      changed-source compilation and a bounded real Windows smoke run.
- [x] Independent review; fix blockers; create/attach PR and merge after checks.
      PR #34 merged as e9fbb95 after all four platforms passed.

Validation: 296 Python tests passed (11 environment skips), followed by all 52
benchmark tests after the final archive/metadata regressions. Changed native
units compiled and linked using verified unchanged cached objects. A bounded
242-second Windows D3D12 run reached Dolphin Resort and exited on the configured
after-speech limit with audio, normal race/UI time, and every draw enabled.
Screenshot verified; 2,968 sampled frames averaged 59.8 fps under the 60 fps cap.
This is a functional smoke check, not a throughput comparison. Private source
hashes, effective settings, save fixtures, and logs are under ignored out.
Independent review found no remaining concrete blockers after fixes.

## 2. Checkpoint interval

- [x] Import ac62398 separately, retain original author; publish after PR #34.
- [x] Carry forward strict parsing and real scheduler/entry tests already
      validated in camera-debug; consider measured Windows256/other32 default.
- [x] Validate current code, cancellation, urgent handoff, platform defaults;
      use prior recorded throughput/input/audio evidence without repeating it.
- [x] Review, create/attach separate PR and merge after checks.
      PR #35 merged as 721364f after all four platforms passed.

Checkpoint integration: Windows default 256, other platforms 32; accepted
overrides 1..4096. Platform-default regression failed before the change and
passed afterward. Real entry/cancellation/urgent tests and all 18 scheduler
groups passed. Changed native units compiled/linked; a separate 242.25-second
D3D12 smoke logged interval 256, reached the race, and ended on its configured
limit with normal time/audio/full drawing. Screenshot verified. Independent
review found no concrete blocker. Existing paired measurements are summarized
with their limitations in docs/checkpoint-interval.md.

## 3. Render thread

- [x] Review 751aacc against updated main and verified resolved-texture lifetime
      correction; inspect queue ordering, resource ownership and failure/exit.
- [x] Add reproductions for concrete defects and minimal fixes if needed.
- [ ] Verify D3D12 device transitions/teardown and Vulkan disabled-default path;
      perform bounded isolated-save functional and performance checks.
- [ ] Review, create/attach separate PR; merge only if required checks pass.
      Report concrete blockers if device/runtime validation prevents merging.

Keep provenance and private smoke assets under ignored out. No game assets in
commits. Fresh checks and exact source refs in each PR description.

Render integration includes resolved-target lifetime prerequisite 726dc62 and
preserves knuckleslee's 751aacc author. Review found failure paths that could
skip waiting for the prior GPU frame or consume an error before cleanup.
Both reproduced and now pass: failures stay sticky through drain, prior-frame
wait and partial-list discard. Worker identity, 6,000-entry queue rollover,
pending-work destruction and exceptions are covered. Presentation and resolution
tests pass on local Vulkan/D3D12; final native units compile and link. Final
same-binary off/on/off comparison is pending; the earlier 48-second aborted
attempt is retained and excluded. No release or device installation performed.
