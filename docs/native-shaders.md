# Original shader creation on the native backend

The original effect parser calls Free Riders `CreateVertexShader` (`824ED770`)
and `CreatePixelShader` (`824ED588`) with a complete Xenos container in `r3` and
expects a resource handle or zero in `r3`. These two public functions are native
boundaries; their original effect callers still execute. The native owner matches
the complete original bytes and shader stage to the locally compiled cache.

The cache uses the Marathon-pinned XenosRecomp translation implementation at
`fb32631ee398e46f2a113d8f9103201dbaa000b4`. Zero-specialization shaders compile to
direct `vs_6_0`/`ps_6_0` stage blobs. Shaders with a nonzero specialization mask
remain `lib_6_3` libraries until observed game state can supply specialization.
For this basic asset, all 34 vertex masks are zero; all 34 pixel masks are `2`
(alpha-test). Pixel creation retains the real library and metadata. It does not
invent a zero alpha-test state or claim a ready pixel pipeline.

Plume's D3D12 `createShader` retains stage bytecode. Actual D3D12 pipeline
validation happens when creating a pipeline state object. Creation alone proves
neither pipeline validity nor a draw, frame, title screen or main menu.

## Original ABI and ownership

Generated source audit is in `out/recomp/diagnostic-subfze/ppc_recomp.77.cpp`:
VS line10740 and PS line10443. Each checks an optional callback before reading
the container (VS global82B50A30, PS82B50A2C). A nonzero callback explicitly
stops until its forwarding ABI is supported. It is not silently skipped.

Effect helper825E4700 (`ppc_recomp.90.cpp:3949`) stores each returned handle,
checks it for zero and advances through variable-size original records. It
does not dereference shader resources. Device AddRef824F44F8 at instruction
825E83A8 (`.90.cpp:5826`) now executes its original body only for the known
native device. Its complete body (`.78.cpp:8166`) loads, increments and stores
the already established BE count at device+3C, and returns the count. It has
no other accesses or calls. Device Release and shader reference operations
remain guarded.

Native shader handles occupy reserved, uncommitted pages from71800000, with
1000hex stride and a current diagnostic capacity of256. Each host record owns
its native stage or retained library, references its immutable complete source
container and begins with reference count1. Handles are not reused. Xbox header
reads fault; the native graphics guard recognizes all allocated shader handles.
No fabricated resource header or original Xbox physical allocation is published.
All resources are reclaimed with the native owner at process teardown.

The following resource operations remain separate dependencies: AddRef824F0E38,
Release824F1F50, GetType824F0AD8, setters824EBD08/824EBB00 and getters824EBED8/
824EBCC0. Original setters interpret default constants through container+20,
update device constant storage and dirty masks, and access driver tracking.
Recording just a pointer would omit those semantics. Their original bodies
remain guarded for native objects until equivalent behavior is implemented.

## Reproduction

```powershell
python scripts/bootstrap.py --verify-only
./scripts/build_shader_translator.ps1
python scripts/prepare_shaders.py
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-lhzu
ctest --test-dir out/build/host -R native_shaders -V
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The preparation defaults use `private/assets/shader/Xbox360BasicShader.fxobj`,
XenosRecomp's dxc-bin DXC (1.8.2407; the Windows SDK's 1.8.2502 was used until
2026-09-22, but the game links these libraries at run time and DXC refuses to
link libraries from another version) and `out/shaders/basic`. The generated cache contains private
original and translated bytes; it is never committed. CMake consumes
`out/shaders/basic/shader_cache_data.cpp` when available. A checkout without a
prepared cache builds an empty cache and explicitly stops on the first shader
request; it cannot use the historical probe outputs automatically.

Preparation publishes the generated source only after every conversion succeeds.
Its manifest records source/output hashes, fixed tool revision, compiler version
and exact arguments. Failed rebuilding preserves a previous complete cache and
returns failure. Re-run the preparation successfully before rebuilding/running
when any inputs or tools have changed.

The native test checks all68 actual entries when the cache is available, exact
byte matching, wrong-stage/mutated input rejection, overflow/unmapped bounds,
retained ownership and inaccessible Xbox headers. Without private data it reports
that only negative checks ran. Same-run boot evidence must additionally prove
that the original effect path actually requested and received those resources.

## Shader-creation checkpoint ade9ca0

`out/native-shaders-addref-boot.log` records all68 original creation requests and
corresponding native resources. First vertex container40019B18/596bytes matches
SHA-1 `0aa2cf2beb066de65d5cb9bb9b26bb1110487192`; returned handle71800000 owns
5676bytes of native stage DXIL. The last pixel container400247C0/484bytes returns
handle71843000 retaining3752bytes of library DXIL, mask2. The original AddRef
then executes and the loader enters its actual critical section82AD440C.

At this checkpoint the stop was `825E47D8: logged_unsupported_instruction`, LR825E8428,
after1419function entries. LAST_FUNCTION824F44F8 is the preceding entered body;
the rejected effect function is not entered. The recompiler log reports its
first missing instruction as `sthu` at825E4954. The next work is checked
translation of those original instructions, not bypassing effect initialization.
There is still no original draw/Present, title or input-to-menu result.


Verification completed: `out/native-shaders-final-build.log`,
`out/native-shaders-final-ctest.log` (25/25),
`out/native-shaders-final-tests.log` (117 Python tests, no skips), and
`out/native-shaders-final-dependencies.log` (all three pins). Runtime and pipeline
received independent specification and quality approval. The original device
AddRef exception was independently audited and reviewed. Negative callback,
handle-capacity and handle-collision paths were inspected rather than directly
exercised; no claim of complete shader binding/lifetime support is made.


Current continuation: `out/lhzu-native-boot.log` also completes original effect
initialization and all four original vertex declarations. Their actual metadata
and elements match the source tables. The continuation now creates and clears real Windows synchronization objects
and stops atExCreateThread, LR824D2518, after1944entries. See
[current initialization evidence](native-sync-objects.md).
