# Exact constant upload reuse experiment

Evidence: `constant-upload-probe-1` finds 61.58% identical stage uploads in the
late race window. User authorized autonomous optimization. Stay isolated from
Claude's timing work and preserve the test2 artifacts and probe executable/map.

Design: one CPU shadow and immutable upload offset per VS/PS constant stage.
Compare all 4096 bytes. On a miss copy to the existing reserved slot and shadow;
on a hit bind the prior offset without writing that slot. Keep ring allocation
and shader ABI unchanged. Invalidate both stages at every after-flush callback,
including synchronous flush and asynchronous present. Never read upload memory.
Use `SFR_CONSTANT_UPLOAD_REUSE=1` as an opt-in experiment, initially off by default.

1. Red/green helper test: initial zero data, repeated bytes, changed bytes at
   end of buffer, signed zero/NaN payloads, immutable old upload, offset zero,
   reset with identical data and new destination.
2. Integrate after potential ring flush, before binding offsets. Add actual
   bytes saved to frame diagnostics. Preserve independent probe semantics.
3. Real GPU test for both VS and PS bytes, independent stage changes, repeated
   draws at different offsets, flush/overwrite/reset and fresh renderer. Exercise
   Vulkan push addresses and D3D12 root descriptors, with reuse off and on.
4. Build Windows and Android, independent read-only review, then alternate
   same-executable reuse-off/on race runs without sampling profiler. Preserve
   game/audio timing, settings, assets, cap and no-overlap guard. Validate saved
   bytes and screenshots. An isolated copy reduction does not prove handheld FPS.
5. Retain only with correctness and credible performance evidence; otherwise
   archive the experiment without enabling it or changing the delivered package.
