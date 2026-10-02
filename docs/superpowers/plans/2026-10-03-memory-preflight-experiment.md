# Ordinary-page write preflight experiment

Status: four-line preflight accepted into this branch after isolated Ally X measurements and independent review. Android regression completed without a material FPS gain. Worker waiting, sorted lookup and constant-copy experiments remain separate and unaccepted. Not released; other-course performance remains unverified.

## Evidence and hypothesis

The exact-map 35-second WPR capture records `GuestMemory::check_store_access` in 1.77% of main-thread leaf samples and `special_word` in 2.31%. Graphics state and texture preflight repeatedly check small ordinary memory ranges. These sampled costs are not an additive or guaranteed speedup estimate.

The existing page flags already distinguish complete ordinary pages from partial, unmapped, import and provider pages. Test whether a whole-range, single-page fast-access check can avoid the general validation scans. Prefer reuse of existing metadata over another cache, registry sorting or a new concurrency protocol.

## Contract

At the beginning of `check_store_access`, only with a null pending-write owner, accept an existing `fast_access` whole page when `page_pinned(address)` is false. Call **the unchanged `mark_written(address, size)`** before returning. Leave every other path unchanged.

Do not use `fast_write`: its partial-page and reservation rules are different. Do not reject a live reservation here: reserved operations also call this preflight, and `check_write` retains its later reservation rejection. Preserve dirty bits, epochs and the watched-write counter, including marking before a reservation error. Owner publication and any pinned page always use the original validation and overlap precedence. `complete_store` and write-combined fences remain unchanged. Existing exclusive layout mutation/pin admission requirements still apply; this is not a new guarantee for concurrent remapping.

## Verification and gate

- [x] Build a differential contract test against the unmodified source first. Record successful baseline output.
- [x] Build a private copied source with only this early return and compare complete observable output: all 1,208 snapshots match, including bytes, dirty/epoch state, counters, provider calls and full RuntimeStop details.
- [x] Cover complete ordinary ranges and top-of-address-space accesses; zero, overflow, cross-page, partial, unmapped, decommit and release cases; import/provider behavior; pending leases and disjoint same-page writes; word/doubleword reservations and failed conditional writes.
- [x] Existing guest memory, pending-write and vector-memory suites pass against both variants. The 202 retained object and 18 local library hashes were checked; the old build cache is unchanged.
- [x] Separate scope and correctness reviews found no blocker for an isolated game experiment. Serial `concurrent_reader` coverage does not establish new concurrent admission guarantees.
- [x] Link a private diagnostic with only the guest-memory object replaced, retaining the original worker budget and timing harness. Complete an Ally X baseline/candidate/baseline sequence with independent saves and bounded normal-time races.
- [x] Accept only a repeatable CPU/main-thread benefit exceeding run drift, without new errors, rendering omissions or altered game time. Review frame tails separately: both candidate P99s improve, but one candidate maximum exceeds the controls. No uniformly improved worst-case latency claim.
- [x] Promote the exact reviewed four-line source and freshly compile four standalone memory tests; all outputs match the original baseline snapshots. These tests do not establish GPU rendering coverage.

No release, merge, external comment or Android performance claim is part of this experiment. A microbenchmark may diagnose local cost but cannot establish gameplay improvement.

## Initial device results and measurement limitation

The untraced I/J/K sequence measured main CPU costs of 9.463 / 9.144 / 9.454 ms per frame and throughput of 74.13 / 76.40 / 72.59 FPS. The candidate's roughly 3.3% main-thread reduction exceeds the main-CPU drift between these two controls. Mean draw count differs by about 0.4%; this is one stationary scene, not a general performance result. All three completed normally. Screenshots in J show the race clock progressing 62.58 seconds over 62.597 seconds of logged time.

Promotion was held after this first sequence: the candidate had one frame above 100 ms, and the second baseline had a 576.8 ms frame. Separate scheduler traces prove that several smaller stalls wait on the diagnostic stderr lock, with the diagnostic `fflush` thread releasing it. They do not establish the cause of every larger stall. Subsequent matched runs use direct inherited disk handles instead of PowerShell output redirection, keeping each game executable unchanged. This is measurement isolation; the production Windows launcher already uses file handles, so the harness change is not a production game optimization.

## Direct-file repeat and acceptance

| Run | Main CPU ms/frame | FPS | P99 frame ms | Maximum frame ms |
| --- | ---: | ---: | ---: | ---: |
| L baseline | 9.6125 | 73.19 | 16.50 | 22.94 |
| M preflight | 9.2180 | 76.19 | 15.89 | 29.31 |
| N baseline | 9.4412 | 74.19 | 16.36 | 24.50 |
| O preflight | 9.1866 | 74.92 | 15.97 | 24.25 |

Adjacent pairs reduce main CPU/frame by 4.10% and 2.70%, versus 1.78% baseline drift. Both candidates beat both controls. Mean vertex bytes and index counts vary approximately 0.21% and 0.23%; draw count varies 2.22%, so workloads are comparable but not identical. All four complete normally with no frames above 33.33 ms in the middle 100-second windows. M's maximum exceeds both baseline maxima, despite better P99s. Adjacent FPS changes are +4.10% and +0.99%; do not promise a fixed FPS gain.

Independent measurement review accepts this narrow main-CPU result for the branch. The source is byte-identical to the reviewed candidate (SHA256 `d1a1c6c0970567f2f1782df91d688b67b50e67f0bed2b0dccb8465e0676600a8`). Fresh verification is saved under `out/build/cpu-memory-production-v047/verification.json`. Raw logs, executable identities, clock anchors, settings, power metadata and screenshots remain private in the four case folders under `out/cpu-profile-v047`. This is one stationary Dolphin Resort scene on one Ally X; neither statistical certainty nor other-device/course coverage is claimed.

## Android follow-up

AYN Thor completed an official v0.4.7 / memory-only candidate / official v0.4.7 sequence using independent saves and caches, full rendering, normal game time and the same fixed frame window 15000–20500. Only `libmain.so` differs in the candidate APK; the other 11 entries are identical. Signature, versionCode 17, API29 minimum and 16 KB alignment were verified. No uninstall or app-data clear was used.

| Run | Main CPU estimate ms/frame | FPS | P99 frame ms | Maximum frame ms |
| --- | ---: | ---: | ---: | ---: |
| A official | 27.2426 | 25.1395 | 49.6516 | 87.4253 |
| B3 memory preflight | 27.0140 | 25.1535 | 50.3039 | 86.6709 |
| C official | 27.2159 | 25.0800 | 50.4246 | 87.3310 |

The candidate's CPU point estimates are about 0.74–0.84% lower, but observed frame-bracket ranges overlap. Those ranges are not strict error or confidence bounds: buffered logs and sequential process/thread reads introduce uncertainty. CPU comes from actual `/proc` ticks with CLK_TCK=100, owned process identities and log/frame anchors, not wall waiting. Workload counters are close but not identical. FPS differences are below 0.3%, so no material Android FPS improvement is established. Each window has zero frames at or above 100 ms; P99 is between the controls. Battery readings do not measure SoC temperature; unavailable thermal HAL data cannot exclude throttling. These are diagnostic, stationary-scene runs, not ordinary-play or all-course performance guarantees.

Two failed launches B/B2 are excluded: an initial private runner restored settings by renaming shell-owned files over app-owned files. The launcher could not write its settings and never started the game. The reviewed V2 runner restored settings in place and performed one byte/metadata-constrained recovery through the launcher's own file creation. B3 and C then completed normally. Final read-only verification confirms official v0.4.7 is installed, both settings retain original bytes and app ownership/RW permissions, the original user-save file list and hashes are unchanged, and no package processes, debug overrides or benchmark lock remain. This was a test harness failure, not a candidate native crash.

Raw cases and the final comparison are under `out/cpu-memory-v047/android-bench`, with `analysis/completed-aba` and `final-device-verification/result.json`. Candidate screenshot clock progression is 191.30 seconds over 191.2997 logged seconds. The failed-launch evidence and frozen V1 runner are retained separately.

## Private normal Windows package

`out/build/cpu-memory-normal-v047/FreeRidersRecompiled-0.4.7-memory-test-windows-x64.zip` is built with normal timing, original worker behavior and only the accepted preflight source change. SHA256: `b090e7433dd7b9dcd7add649391a68ace399071d97b3d65a2efd05388bf5865d`. CRC and payload comparison passed: only the runtime executable differs from the official release ZIP. This complete package has not itself been launched; gameplay measurements above use the separately identified diagnostic build. It is private, not a published release.
