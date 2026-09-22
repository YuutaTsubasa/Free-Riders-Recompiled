# Native coherence for original CPU texture access

The Windows original title/menu goal remains **unachieved**. The previous
checkpoint5701d32 reached original texture transfer but stopped reading
`VdGlobalDevice`. The new work connects the required coherence operation to the
native queue while preserving the original CPU resource lifecycle.

## Original request and replacement boundary

Actual original824F1C08/LR824F3560 requests resource400007C0, null64-bit token,
operation14, base/rangeF8AFC000 with size2000, no secondary base or options, and
coherence mask02000000. The resource is a guest texture header, distinct from
destination surface4002FEA0. Its initial words are100003,2,0,0,0,FFFF0000,
FFFF0000,1000002. Original code atomically adds100 to the header before coherence.

The55-instruction block[824F1D88,824F1E64) loads the original device global,
checks device ownership, and emits coherence and wait commands or merges a
deferred range into device synchronization state. Pinned Xenia register and
command definitions identify COHER_SIZE_HOST/BASE_HOST/STATUS_HOST and the
texture-cache action bit; WAIT_REG_MEM waits for the busy bit to clear. The
original physical alias conversion maps the virtual request to18AFD000..18AFF000.
This operation is not a texture upload or draw. Primary definitions:
[register table](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/register_table.inc),
[coherence fields](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/registers.h),
[command processor](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/command_processor.cc).

The generator admits only the exact original whole-function canonical-LF SHA256
`cb1c6b2ed0c4104235335296d550032ee67ec39cac16006b35e5401e7bf4564a`.
It checks the block range, ending label and branch edges, then substitutes one
native callback after ordinary instruction rewrites. All210 original comments
remain as provenance; the55 comments inside the native block do not mean those
Xbox commands execute. Code outside the block and the ending checkpoint remain.
There is one external fallthrough entry and no branches into internal labels;
removed register/condition writes are dead before later use or restored by the
original epilogue. A changed function fails closed.

Original header increment, dirty-range handling, CPU pointer return, texture copy
and unlock are retained. Original unlock8253E6C8->824F22B8->824F0B68 subtracts100
from the header. The native callback does not write guest data, registers,
header fields or a fabricated command ring.

## Ownership and native completion

The callback obtains the original saved caller at currentSP+184 and coherence
mask atSP+284, along with the original nonvolatile arguments. It validates the
audited caller/mode, first-lock state, reference/fence/dirty fields, and the full
52-byte resource header. Packed backing words+32/+48, masked withFFFFF000, must
match the actual requested bases. This prevents a plausible header from being
paired with a different valid allocation.

The complete nonempty range must be writable and contained in one allocation
owned by PhysicalMemory. Each allocation explicitly starts CPU-only. Marking any
part GPU-backed conservatively marks the whole allocation; malformed or crossing
marks leave metadata unchanged. GPU-backed allocations are rejected here because
queue idleness alone cannot refresh a native texture cache. Future GPU resource
publication must mark backing before publication and consume the latest CPU
bytes after the original access completes; this helper supplies no such upload.
The [pinned Unleashed backend](https://github.com/hedge-dev/UnleashedRecomp/blob/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c/UnleashedRecomp/gpu/video.cpp)
performs real upload/copy work in the texture unlock lifecycle; queue completion
must not be mistaken for that work.

NativeGraphics owns a real D3D12 fence with increasing signal values. A queue
signal orders completion after submitted work; checked fence polling verifies
completion, cancellation, device removal, value exhaustion and a five-second
timeout. Two-millisecond polling avoids an event whose handle could close while
completion is pending. The guest execution permit stays held across validation
and waiting to prevent ownership races; cancellation is checked again before
reporting success. Pending native work cannot access this CPU-only backing.

Unsupported modes/ownership stop before submission. A native failure propagates
without a success return. The original header increment already occurred, so
execution aborts rather than retrying the original function or inventing unlock.

## Verification evidence

Physical ownership, resource-coherence and generator tests failed before their
implementations and passed afterward. Review found the independent resource/base
association gap; its regression failed before the additional header validation
and passed afterward. Real GPU tests submit four fresh copies without a caller
completion fence and compare every readback byte after wait_idle. Pre-cancellation
also failed before implementation and passes afterward.

The72-case generator suite passes. Fresh out/recomp/diagnostic-coherency contains
one audited native block, with unchanged original input/log fingerprints and
function retention:31,689 retained and14,814 rejected. Details are in
out/coherency-generation-comparison.json. The original-flow audits are
out/vd-global-device-next-audit.md and out/resource-coherence-original-audit.md,
with machine-word hashes and control-flow evidence in matching JSON files.

VdGlobalDevice itself remains an unsupported import. Its ABI requires a separate
writable zero-initialized pointer cell; publishing the opaque native token would
not establish the device ownership/ringbuffer layout consumed by original code.
Replacing the required coherent-access operation avoids that incompatible device
representation without claiming the entire export or device layout is implemented.

## Actual original transfer completion

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-coherency
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-coherency
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

Generation requires a fresh output directory. The resulting Windows executable
is out/build/host/sfr_cpu_diagnostic.exe. The same original-entry run in
out/coherency-final-boot.log exits3 with:

```text
NATIVE_RESOURCE_COHERENCY resource=0x400007c0 address=0xf8afc000 size=0x2000 mask=0x2000000 ownership=cpu-only queue_completed=1
ORIGINAL_TEXTURE_TRANSFER_RETURN surface=0x4002fea0 caller=0x8250e718 result=0
ORIGINAL_TEXTURE_UNLOCK_STATE resource=0x400007c0 header=0x100003
STOP import-variable @0x82000664: __imp__VdGlobalDevice
LAST_FUNCTION sub_824F19E8 @0x824f19e8 LR=0x824f1a04 calls=3499
```

The read-only return observer captures entry SP/surface at8250C848/LR8250E6E8.
It observes824F1F50/LR8250E718 only with the same SP/surface, after the caller
has received the original return and rejected negative results. Original r30
contains0. The resource header has returned from100103 to100003 after the
original unlock. Neither result nor header is supplied by the observer.

The import name repeats, but the new stop is in original resource destruction
824F19E8 after successful transfer, rather than the earlier coherence block.
Entry counts vary with worker scheduling. This proves original CPU transfer
success and balanced lock state; it does not yet prove native GPU texture upload
or its byte layout. No original draw, Present, title screen, confirmation input
or main-menu transition is established.

All37 native CTest and152 Python tests pass without skips, including real-entry
transfer/unlock assertions. Independent source, generator, ownership and native
fence reviews found no remaining actionable findings. Main logs:
out/coherency-final-build.log, out/coherency-observer-build.log,
out/coherency-tests-build.log, out/coherency-ctest.log, out/coherency-python.log,
out/resource-association-red.log, out/resource-association-green.log,
out/native-idle-red.log, out/native-idle-green.log,
out/native-idle-cancel-red.log, out/native-idle-cancel-green.log,
out/physical-cpu-owner-red.log, out/physical-cpu-owner-green.log,
out/resource-coherency-red.log, out/resource-coherency-green.log,
out/coherency-generator-red.log, out/coherency-generator-green.log,
out/coherency-generator-suite.log.
