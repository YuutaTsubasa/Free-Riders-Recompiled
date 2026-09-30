# Constant upload reuse investigation

The user authorized autonomous performance investigation while away. Keep the
private test2 candidate unchanged, work only in the Codex checkout, and do not
change Claude's scheduling/timing implementation.

Evidence: current main-thread profile has 22,534 samples, including native draw
work and memcpy; late unprofiled record time is about 2.49 ms per frame. Every
draw uploads 4 KiB of vertex and 4 KiB of pixel constants even when unchanged.
This motivates measurement, not a presumed cache benefit.

Alternatives: shorten buffers based on shader reflection (changes interfaces),
track guest constant writes (requires complete mutation coverage), or compare
the immediately preceding uploaded bytes. Investigate the last option first:
it is bit-exact, local to the renderer, and can invalidate at every upload-ring
flush without guessing guest state.

1. Add opt-in `SFR_CONSTANT_REUSE_TRACE=1` counters to `native_renderer.cpp`.
   Compare complete CPU-side arrays, not floating-point values or GPU mappings.
   Reset previous-value validity on every presentation flush. Count uploads
   and identical-stage bytes without skipping/changing any actual GPU write.
2. Expose counters alongside PipelineWork and existing frame metrics. Allocate
   comparison buffers only when enabled; disabled mode adds no comparisons,
   CPU shadow copies, clock reads or per-draw output.
3. Build Windows runtime and affected graphics tests. Collect the same 11,500
   present race, no overlapping process. Verify uploaded bytes equal draws *
   8192 and reusable bytes never exceed uploads, and inspect the race image.
4. If enough bytes repeat, consider an isolated reuse experiment with explicit
   ring lifetime/invalidation tests and Vulkan/D3D12 rendering checks. Require
   actual reduced work and non-profiled A/B evidence before enabling a cache.
   Otherwise retain findings, not a speculative runtime cache.

The probe's comparison overhead means its frame timing is not a speedup metric.
Desktop success is not handheld acceptance or an Xenia performance comparison.
