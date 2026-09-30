# Vertex swap pointer optimization

Goal: reduce measured main-thread vertex byte-swap cost without changing data,
draws, image quality, game timing, instruction-set requirements or Claude's files.

Evidence: main-cpu-profile-1 sampled swap_words_into in 1,007 of 23,006 samples
(4.38%). Disassembly of f49e3253 showed source/destination span pointers reloaded
on each 16-byte iteration. A standalone local-pointer variant produces identical
buffers and lower aggregate timings across tested WB/WC sizes and alignments.
Short microbenchmarks are noisy and cannot establish game FPS benefit.

Implementation: cache source/destination .data() once at entry. Keep existing
SSSE3/NEON/scalar loops, common-prefix size, and tail behavior. No restrict alias
promise, wider ISA, streaming stores or graphics settings changes.

Validation steps:
- Characterize independent scalar expected bytes, SIMD/tail lengths, both
  pointer alignments, source immutability and destination guard bytes before edit.
- Preserve old executable/map and rerun those tests after the minimal change.
- Build runtime; compare the same race with original and optimized executables,
  reject overlap and preserve normal timing. Inspect screenshots.
- Keep conclusions proportional to sample variability; report microbenchmark
  and game results separately. Hardware acceptance remains required.

Completed: scalar/bounds characterization before and after; native runtime
build; three targeted CTests; disassembly confirms removed pointer reloads;
original game baseline reached 11,500 presents with no overlap.
Pending: modified full-race comparison and screenshots. Its first attempt was
automatically stopped at frame 7,843 when Claude started a benchmark. Do not use
that aborted run to assess speed or stability. Claude's next benchmark was
observed live as PID 38680 after PID 47808 completed; do not assume a stale PID
or this note grants the test machine to Codex.

Follow-up: swap-optimized-2 completed 11,500 presents with no overlap and no
dropped waits. Inspected frame-11,250 alongside original; track/character/HUD
render, but race states differ. Three targeted CTests passed again. The late
window averaged 17.6087 ms versus original 17.9204 ms, with different draw counts
and higher measured draw cost; no stable FPS gain is established by this pair.
The scripted capture covers early racing, not a completed lap. Repeated paired
measurements and handheld acceptance remain outstanding.
