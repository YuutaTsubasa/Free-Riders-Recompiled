# Texture conversion performance

Goal: reduce synchronous texture conversion CPU work without changing uploaded
bytes, rendering order, cache invalidation, or texture lifetimes.

Evidence: the private texture phase probe times real uploads; early uploads
spend 0.3-0.7 ms in byte swapping/untile. ARM64 assembly confirms endian=1
uses scalar byte loads/stores and untile calls variable-size memcpy per block.
Wait for the full Loading probe before attributing whole-frame stalls.

Plan:
- Measure texture stages through Loading/racing; retain baseline symbols/APK.
- Add exhaustive endian span/tail tests and untile reference tests for 4/8/16
  byte blocks, partial rows, padded pitch, truncated input and unaligned data.
- Benchmark original transforms on ARM64 before implementing changes.
- Use 16-byte SIMD permutations for endian modes 1 and 3, with scalar tails;
  dispatch fixed-size untile copies for supported 4/8/16-byte blocks, retaining
  the generic fallback. No asynchronous resource lifetime change.
- Run host/ARM64 tests and pair old/new conversion timings with byte equality.
- Run the Thor fixture with a private per-upload verifier, then normal build.
- Restore normal APK, document actual improvement and remaining long frames.


Investigation updates:
- Completed phase probe has 1,013 actual uploads. Conversion totals are
  36.1 ms swapping and 80.7 ms untiling; upload fence waits total 134.2 ms.
  The largest upload waits are 33.9/18.0/12.1 ms, at the fifth upload of
  their respective frames. Allocation totals only 8.9 ms. Do not introduce
  a staging pool based on the earlier coarse timing outlier.
- Four upload command lists impose a wait on reuse. Increasing this ring
  could just move the wait to presentation; batching needs submission-order
  and resource-lifetime validation. This change retains the existing ring.
- Upload waits currently call the queue directly, unlike presentation waits
  that may release the guest execution permit. Reusing that wrapper would
  require auditing renderer reentry and guest-memory lifetimes first.
- Exhaustive host and ARM64 native-format tests pass; Android and Windows
  production targets build. Source review found no actionable findings.
- Standalone ARM64 old/new microbenchmarks verify identical bytes. Endian 1
  changes roughly 36-45 to 6 ms; endian 3 roughly 20 to 6 ms. Untile 4/8/16
  byte blocks changes roughly 19/22/33 to 8/15/26 ms. These are repeated
  transform timings, not whole-frame or FPS claims.

Completed validation:
- Live verifier completes 14,400 presents: all 999 uploads match byte for
  byte, 159,179,776 source bytes. Swap totals 31.929 -> 5.800 ms; untile
  totals 74.037 -> 44.082 ms. No unchanged dirty texture was observed.
- Normal APK completes the same fixture and retains race graphics. Maximum
  frame after present 9000 remains 181.634 ms, including 62.361 ms texture
  work. Scene alignment differs; do not compare maxima as equal workloads.
- Normal APK installed on Thor, diagnostic override removed and launcher
  restored. Detailed results and limitations are in docs/performance.md.
