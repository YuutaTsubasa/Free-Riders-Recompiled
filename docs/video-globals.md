# Original shared-surface destruction and texture creation

The Windows title/menu goal remains **unachieved**. This checkpoint continues
past the shared-surface destruction that followed original CPU texture transfer.
Two original texture-creation calls now return success; native GPU texture upload
and game rendering are still not established.

The [pinned Xenia export initialization](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc)
defines VdGlobalDevice as a separate writable four-byte cell initialized to zero.
The IAT contains the cell address, not a device pointer. VideoGlobals provides
this storage at32-byte-aligned72500000. The cell remains writable; no native
device token or original device layout is fabricated.

Only verified import82000664/nameVdGlobalDevice/ordinal446 is bound. Its provider
checks native thread/context ownership, original function824F19E8 and LR824F1A04,
saved caller at currentSP+104 equal824F1FB8, aligned resource r31, shared-surface
header predicate `(header & 0x4000000F) == 0x40000004`, and zero refcount. Other
consumers stop explicitly. The helper checks only the eight bytes actually
needed; it does not access a parent that the original recursive Release may
already have released. It returns the real cell address regardless of its value.

Actual surface4002FEA0 has header40100004, refcount0 and parent400007C0. Its
original destructor unconditionally follows the shared-surface branch to CPU
header free. That branch ignores the loaded device and does not free parent
texture backing. Original refcount atomics, parent Release and heap free remain
untouched. The complete original destructor byte hash and branch audit are in
out/resource-release-next-audit.json and out/resource-release-next-audit.md.

## Actual evidence

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-coherency
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The same-run log out/video-globals-final-boot.log includes:

```text
VIDEO_GLOBAL_READ cell=0x72500000 value=0x0 resource=0x4002fea0 caller=0x824f1fb8
ORIGINAL_SHARED_SURFACE_HEAP_RESULT resource=0x4002fea0 result=1
ORIGINAL_TEXTURE_CREATE_RETURN result=0 output=0x701320a4 texture=0x400007c0
ORIGINAL_SHARED_SURFACE_HEAP_RESULT resource=0x4002fef0 result=1
ORIGINAL_TEXTURE_CREATE_RETURN result=0 output=0x701320ac texture=0x4002fea0
ORIGINAL_VIRTUAL_FREE_REQUEST base_ptr=0x70130a94 size_ptr=0x70130a90 type=0x4000 base=0x40070000 size=0x10000
STOP import-function @0x82acb98c: __imp__NtFreeVirtualMemory
LAST_FUNCTION sub_824DF6A8 @0x824df6a8 LR=0x824e0dd8 calls=4391
```

The first two original DDS states are64×64 and128×128 DXT5. Both transfer and
unlock complete, their temporary surface headers reach original heap free, and
E188 texture creation returns0 with original texture output pointers. The third
256×256 texture reaches a64KiB decommit request at40070000 and stops there.
The real video-global cell adds one backing page: kernel pages rise from13 to14
and available pages fall by one. Original heap sizing therefore requests7201000
bytes atF8AFF000; the first texture backing is nowF8AFD000. Regression expectations
follow this real accounting change rather than forcing the old physical addresses.

Heap result observation captures824E1EE0/LR824DF404 with r5 equal the surface,
then reads original r3 at rest26/82A560B0 with LR824E2150 and the same entry SP.
This is the computed heap result before its epilogue, not a substitute return.
The creation observer captures E188 entry SP/LR/output slot; at rest24/82A560A8
with the same LR and SP=entrySP+224, E188 has returned and r3 is its actual result.
It rechecks the output slot. These observers never write results or guest memory.

All38 native CTest and152 Python tests pass without skips. The unit regression
failed before lookup implementation and passed afterward. Tests cover exact
writable BE cell bounds, occupied-cell preservation, all context fields,
shared/refcount predicates, truncated/null/misaligned headers and no parent read.
Existing generation is unchanged. Logs: out/video-globals-red.log,
out/video-globals-green.log, out/video-globals-final-build.log,
out/video-globals-observer-build.log, out/virtual-free-trace-build.log,
out/video-globals-ctest.log and out/video-globals-python.log.

No original draw, Present, title screen, confirmation input or main-menu transition
has occurred. Texture creation returning successfully is not native GPU upload.
