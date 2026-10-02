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
- [ ] Independent review; fix blockers; create/attach PR and merge after checks.

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

- [ ] Import ac62398 separately on the updated main, retain original author.
- [ ] Carry forward strict parsing and real scheduler/entry tests already
      validated in camera-debug; consider measured Windows256/other32 default.
- [ ] Validate current code, cancellation, urgent handoff, platform defaults;
      use prior recorded throughput/input/audio evidence without repeating it.
- [ ] Review, create/attach separate PR and merge after checks.

## 3. Render thread

- [ ] Review 751aacc against updated main and verified resolved-texture lifetime
      correction; inspect queue ordering, resource ownership and failure/exit.
- [ ] Add reproductions for concrete defects and minimal fixes if needed.
- [ ] Verify D3D12 device transitions/teardown and Vulkan disabled-default path;
      perform bounded isolated-save functional and performance checks.
- [ ] Review, create/attach separate PR; merge only if required checks pass.
      Report concrete blockers if device/runtime validation prevents merging.

Keep provenance and private smoke assets under ignored out. No game assets in
commits. Fresh checks and exact source refs in each PR description.
