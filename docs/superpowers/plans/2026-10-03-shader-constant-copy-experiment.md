# Shader constant copy experiment

Status: private experiment completed; not accepted because the candidate falls between its controls. Production constant-copy code remains unchanged. No game timing, rendering content, submission order, release or publication change is part of this experiment.

## Evidence and design

The existing exact-map Windows baseline records 480 of 23,940 main-thread samples (2.01%) in the two 4,096-byte shader-constant copies and following in-place endian swaps. This is sampled cost, not an expected saving. Both blocks run during ordinary play with frame metrics disabled.

Reuse `swap_words_into` to copy and swap each constant block in one pass. It already supplies x64 SSSE3 and ARM NEON paths. Retain the full `memory.check` before accessing each source, offsets 1920 and 6016, destination sizes, call order, metrics boundaries, and every downstream draw field. Source guest memory and destination host arrays are distinct; no new cache, ISA requirement, lifetime or synchronization rule is introduced.

Alternatives considered: retain the existing two-pass operation if no benefit is measured; add a constant cache, which requires broader invalidation proof and is outside this narrow experiment.

## Tasks and acceptance gate

- [x] Verify disjoint source/destination and existing helper semantics, including its scalar fallback, unaligned addresses and full block bounds.
- [x] Compare the exact old and new 4096-byte transformations over varied data and alignments, including guard bytes and source immutability; run existing native format tests.
- [x] Create a private candidate from the frozen benchmark graphics source. Keep the accepted memory preflight object in both baseline and candidate. Preserve original worker settings and the same normal-time benchmark harness.
- [x] Record full source/input/binary hashes and verify only the graphics object differs from the accepted-memory baseline.
- [x] Obtain separate scope and correctness reviews before device use.
- [x] Run a bounded, isolated baseline/candidate/baseline comparison on an available Windows device. Compare CPU/frame and rendering workload, inspect tails and normal game time. Keep CPU and wall waiting distinct.
- [x] Keep the result private because a repeatable useful benefit was not established. Do not combine it with the Android memory-only experiment.

Do not overwrite prior binaries, build caches, release artifacts or device settings. An equivalence test or microbenchmark alone cannot establish gameplay speedup.

## Results

Correctness validation passed: 5,120 complete-block alignment/data cases, guard-page checks, 45 real GuestMemory validation/order observations, and existing native format tests. These execute extracted callsites and the actual helper library; they do not constitute full-renderer coverage. The private executable is SHA256 `d398fa1ade72d683f74746fee88649fe04d3d67925bf93dd4bad5fceb0ce3930`. The retained 202 object and 18 local library hashes are recorded with the private link provenance.

| Ally X case | Main CPU ms/frame | FPS | P99 frame ms | Maximum frame ms |
| --- | ---: | ---: | ---: | ---: |
| R accepted-memory baseline | 9.3814 | 74.12 | 16.22 | 24.29 |
| S constant-copy candidate | 9.3045 | 74.48 | 16.16 | 24.42 |
| T accepted-memory baseline | 9.1543 | 76.16 | 15.87 | 51.84 |

The candidate saves 0.82% main CPU/frame relative to R but costs 1.64% more than T. Baseline main-CPU drift is 2.42%, larger than the apparent first-pair benefit. Candidate draws/frame are 0.47–0.67% higher than the controls, so workloads are comparable but not identical. Independent review agrees that a reproducible benefit has not been established; this does not prove the saving is zero. The production callsite remains unchanged. T alone has one frame over 50 ms; one observation does not establish a candidate tail-latency benefit.

All three used separate saves, unchanged full rendering, the same power mode and bounded normal-time race. Candidate screenshots advance the HUD by 65.19 seconds over 65.1732 logged seconds. Raw logs, settings, binary hashes, clock anchors, CPU samples and workload counters remain in `out/cpu-profile-v047/directlog-constants-*`; the comparison is `out/cpu-profile-v047/comparison-constants.json`. All owned game processes and scheduled tasks were removed; WPR is off and the temporary wake request was released. No production source, release or Android constant-copy change was made.
