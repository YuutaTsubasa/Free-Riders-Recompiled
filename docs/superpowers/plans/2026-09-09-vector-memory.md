# Checked full-vector memory

Continue the user-authorized sequential native CPU bring-up from the reservation checkpoint. Current real stop is sub_82AC9470, containing lvx128/stvx128. Preserve the original initializer and translated register operations.

## Contract

Recognize only pinned XenonRecomp full lvx/lvx128/stvx/stvx128 emissions. Effective addresses wrap to 32 bits then align down to 16 bytes. The generated register representation reverses all 16 guest bytes, as verified in the pinned emitter and VectorMaskL. Validate that input header's mask prefix before publishing any vector rewrites. Do not admit element/partial vector memory instructions or unmatched emissions; mask-producing lvsl/lvsr are not memory accesses.

Precheck the complete 16-byte range before access. Loads return a complete temporary before altering register bytes; stores precheck writable range before any bytes change. Respect guards, logical mapping tails, computed read-only words and active reservations. Read providers once per overlapping word. No concurrency/atomic-vector guarantee and no claim that remaining SIMD arithmetic is verified.

## Steps

- [x] Add focused native component/tests for reversed byte order, all alignment offsets, top-of-address-space access, bounds/guards, read-only/reservation failures without partial writes, provider sampling/failure, and source preservation. Observe RED then GREEN.
- [x] Add strict generator fixtures including CRLF, zero/nonzero RA, VMX register limits, mismatches, unsupported variants, unchanged inputs, missing/wrong mask. Observe RED then GREEN.
- [x] Wire checked adapters and build/test targets; generate a fresh diagnostic-vectors directory and execute the real image.
- [x] Run independent specification then quality review; resolve findings.
- [x] Run complete Python/native tests and pinned dependency verification; record actual next stop and limitations; commit source/docs locally.

Evidence: pinned XenonRecomp recompiler.cpp full vector emitters and ppc_context.h VectorMaskL prefix 15..0; IBM PowerPC vector addressing documentation (redp3890, section 2.2). ROM/generated files remain ignored. Windows is the only host executed here.
