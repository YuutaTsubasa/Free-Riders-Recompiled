# Processor selection during original thread creation

Checkpoint 2e6acca reaches ExCreateThread flags 08000001, startup 824D2A10,
worker 824D4388, argument 82B4FFC8. Implement only its necessary ABI extension:
suspended creation plus a zero or single-bit upper-byte logical CPU mask (0..5).
The existing zero-mask parent inheritance remains. Unsupported low flags,
multi-bit masks and nonexistent processors reject before allocation/publication.

Pinned Xenia xthread.cc decodes creation_flags >>24 through GetFakeCpuNumber
and sets both PCR/KTHREAD current_cpu plus native affinity before resume.
Microsoft Coding for multicore documents child inheritance absent explicit
selection. Our native single-processor adapter already maps six guest IDs onto
allowed host processors; reuse it, preserve all original flags in KTHREAD+16C,
and keep the new thread parked until the original resume request.

1. Add tests for all six explicit masks, differing parent CPU, original flags,
   PCR/KTHREAD identities, real host affinity, suspension and rejected flags
   without side effects. Run RED on current implementation.
2. Decode/validate flags before allocation; use the selected CPU consistently
   for original storage initialization and native affinity before publication.
3. Run GREEN native tests, build diagnostic and observe the true-ROM next stop.
   Update boot regression only from actual trace, retaining existing evidence.
4. Independent spec then quality review, full native/Python tests, documentation
   and local commit. Original title/menu remains the active unachieved goal.
