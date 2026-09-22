# Original shader intake investigation (2026-09-09)

The supplied `private/assets/shader/Xbox360BasicShader.fxobj` contains 68 structurally validated Xenos shader containers (34 vertex, 34 pixel). All 68 have actually been extracted, translated with the pinned Marathon XenosRecomp implementation, and compiled to DXIL libraries, with zero reported failures. One original pair has also been linked to vertex/pixel stage binaries using a source-derived zero-specialization test value. This establishes a usable native shader conversion path; it does not establish a rendered frame or correct guest bindings.

## Asset and container evidence

- Asset size: **93,676 bytes**, SHA-256 `b4a865e23ecb7eed2770233d9b865923180d084a483b6d2f0c44cf54d73848b2`.
- First three big-endian words: `A3D70141 00016DE0 0000C6E0`. The second equals file size minus 12. The semantic meaning of the outer effect fields, including the third word, remains unproven; they are not used to infer shader boundaries.
- Embedded records use flags `102A1101` (vertex) or `102A1100` (pixel), exactly matching the upstream `ShaderContainer` schema. Each record's preceding big-endian word equals its verified virtual-plus-physical length. All 68 records are non-overlapping and have distinct full-container SHA-256 hashes. There are 66 eight-byte gaps and one sixteen-byte gap between consecutive records.
- Shader containers total 44,140 bytes; their declared instruction blocks total 12,768 bytes. All reflected targets match stage (`vs_3_0` / `ps_3_0`), with creator string `2.0.11775.3`.

| Record | Container range (end exclusive) | Bytecode range | Container bytes |
|---|---|---|---:|
| First vertex | `0x1124..0x1378` | `0x12DC..0x1378` | 596 |
| First pixel | `0x6724..0x6818` | `0x67F4..0x6818` | 244 |
| Last pixel | `0xBDCC..0xBFB0` | `0xBF5C..0xBFB0` | 484 |

The ignored `out/shader-intake-validate.py` probe follows the pinned upstream structures rather than accepting magic-byte matches. All 68 candidates pass container/file bounds, reserved scanner fields, constant-table records and strings/types, definition-table physical constant ranges, vertex/interpolator array bounds, instruction lengths divisible by 12, and decoded control-flow EXEC clause bounds with an END opcode. Observed control-flow opcodes are NOP, EXEC, EXEC_END, LOOP_START, LOOP_END and ALLOC. The probe does not fully validate loop targets or all ALU/fetch semantics. Metadata is saved in `out/shader-intake-upstream/validation.json`; no extracted asset bytes are tracked.

## Pinned implementation and runtime contract

Local reference commits were verified with `git rev-parse HEAD` and their shader gitlinks with `git ls-tree HEAD tools/XenosRecomp`:

| Reference | Reference commit | XenosRecomp repository and gitlink |
|---|---|---|
| Marathon | `bd9c0bbd8a99bcc2c0fabdf9521462e75e0ae7d8` | `sonicnext-dev/XenosRecomp`, `fb32631ee398e46f2a113d8f9103201dbaa000b4` |
| Unleashed | `cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c` | `hedge-dev/XenosRecomp`, `990d03b28a27b50277ee5d8d942e1c5f873869d1` |

The actual probe uses the [Marathon-pinned translator](https://github.com/sonicnext-dev/XenosRecomp/tree/fb32631ee398e46f2a113d8f9103201dbaa000b4). Source files fetched from that exact commit remain under ignored `out/shader-intake-upstream/`.

- [`shader.h`](https://github.com/sonicnext-dev/XenosRecomp/blob/fb32631ee398e46f2a113d8f9103201dbaa000b4/XenosRecomp/shader.h) defines nine big-endian container words: flags, virtual size, physical size, fieldC, constant-table offset, definition-table offset, shader offset, field1C, field20. Bytecode begins at `container + virtualSize + Shader.physicalOffset`, and `Shader.size` is its byte length.
- [`main.cpp`](https://github.com/sonicnext-dev/XenosRecomp/blob/fb32631ee398e46f2a113d8f9103201dbaa000b4/XenosRecomp/main.cpp) scans directory inputs for containers, hashes full virtual-plus-physical records with `XXH3_64bits`, translates and builds compressed shader caches. Its scanner alone performs fewer checks than this probe. Single-file mode expects one **complete shader container**, not the outer fxobj or bare instruction bytes.
- [`ShaderRecompiler::recompile(const uint8_t*, const std::string_view&)`](https://github.com/sonicnext-dev/XenosRecomp/blob/fb32631ee398e46f2a113d8f9103201dbaa000b4/XenosRecomp/shader_recompiler.cpp) receives a complete container and the contents of `shader_common.h`; outputs are `out`, `isPixelShader`, and `specConstantsMask`. There is no input-length parameter; validate before calling it.
- Marathon's `MarathonRecompLib/CMakeLists.txt` configures input as extracted shader directories and output as `shader_cache.cpp`; Unleashed configures its decompressed archive/private directory. Both are build-time conversions. Marathon `gpu/video.cpp:5564` hashes the original guest container by the same virtual-plus-physical length to look up native shader cache entries.

## Verified translation and exact reproduction

The ignored harness compiles the unmodified pinned `shader_recompiler.cpp` and associated headers with LLVM 21.1.8 and the existing `tools/XenonRecomp/thirdparty/fmt/include` headers (`FMT_HEADER_ONLY`). Its minimal injected PCH retains the upstream endian helpers and omits unrelated DXC/cache headers. No game-specific `MARATHON_RECOMP` or `UNLEASHED_RECOMP` macro is enabled. This is a translation-only dependency reduction, not a change to shader translation logic.

```powershell
./out/shader-intake-build.ps1
./out/shader-intake-harness.exe out/shader-intake-vertex.bin out/shader-intake-vertex.hlsl out/shader-intake-upstream/shader_common.h
./out/shader-intake-harness.exe out/shader-intake-pixel.bin out/shader-intake-pixel.hlsl out/shader-intake-upstream/shader_common.h
$shaderDxc = 'C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'
& $shaderDxc -T lib_6_3 -HV 2021 -all-resources-bound -Wno-ignored-attributes -Qstrip_reflect -Qstrip_debug -Fo out/shader-intake-vertex.dxil out/shader-intake-vertex.hlsl
& $shaderDxc -T lib_6_3 -HV 2021 -all-resources-bound -Wno-ignored-attributes -Qstrip_reflect -Qstrip_debug -Fo out/shader-intake-pixel.dxil out/shader-intake-pixel.hlsl
```

All above commands succeeded with DXC `1.8.2502.11 (239921522)`. Vertex HLSL/DXIL sizes are 22,787/4,536 bytes; pixel sizes are 21,101/2,800 bytes. All six container/HLSL/DXIL artifact hashes are recorded in ignored `out/shader-intake-upstream/translation-manifest.json`. DXIL SHA-256 values are `ac49c69ee0c8ca1e1711e60978f1dc9d42206ef3dd12e547411e840e7ec29482` (VS) and `2ae7fdc8a2e763ae3e3eec7d656199572cd2057cc97e75c4c234b77d1caba77d` (PS).

The above outputs are DXIL libraries, not ready PSO stage blobs. Upstream [`dxc_compiler.cpp`](https://github.com/sonicnext-dev/XenosRecomp/blob/fb32631ee398e46f2a113d8f9103201dbaa000b4/XenosRecomp/dxc_compiler.cpp) uses `lib_6_3` when specialization is needed, otherwise a direct stage profile. The full conversion reveals all 34 VS masks are zero and all 34 PS masks are `2` (`SPEC_CONSTANT_ALPHA_TEST`). The first VS also compiles directly with the exact upstream flags (`-T vs_6_0 -E shaderMain -HV 2021 -all-resources-bound -Wno-ignored-attributes -Qstrip_reflect -Qstrip_debug`). An earlier stage compile without those exact flags failed on `g_SpecConstants`; that failure does not demonstrate that the VS requires specialization. Marathon `gpu/video.cpp:4338` compiles an exported `g_SpecConstants()` constant function, registers both libraries with `IDxcLinker`, and links `shaderMain` to `vs_6_0`/`ps_6_0`. Guest state must determine specialization values for actual rendering.

### Complete 68-shader conversion

`python out/shader-intake-all.py` produced **68 successes / 0 failures**: 34 VS and 34 PS, totaling 1,657,378 HLSL bytes and 308,280 DXIL-library bytes. The extraction validates the source asset and each full-container SHA-256 against the earlier bounded manifest before invoking the translator. The runner checks process exit status, stage, reported HLSL size, DXC status and output signature, records failures without accepting their DXIL, and retains separate translation/compiler logs. Assertions remain enabled in the translator. This detects reported unsupported operations and compiler rejection, but does not prove every instruction's semantic accuracy.

The per-container manifest is `out/shader-intake-all/manifest.json`; each result records status, source offset/stage/hash, tool pin, specialization mask, HLSL and DXIL sizes/hashes. Artifacts live in one ignored directory per source offset/stage. No translator changes, game macros, hand-written shader replacements or raw asset additions to tracked files were used.

### Actual specialization link probe

The pinned shared header gives Vulkan specialization constant zero an initial value of `0`; its Metal fallback also uses `0` when the function constant is absent. That source-derived value was used only as a **link test**, not an observation of FreeRiders' alpha-test state. It disables the source-defined alpha-test specialization bit. The resulting HLSL helper follows the exact reference renderer's exported-function convention:

```hlsl
export uint g_SpecConstants() { return 0; }
```

Using the `$shaderDxc` path above, these commands succeeded:

```powershell
& $shaderDxc -T lib_6_3 -Fo out/shader-intake-spec-zero.dxil out/shader-intake-spec-zero.hlsl
& $shaderDxc -link 'out/shader-intake-spec-zero.dxil;out/shader-intake-all/001124-vertex/shader.dxil' -E shaderMain -T vs_6_0 -Fo out/shader-intake-linked-vertex.dxil
& $shaderDxc -link 'out/shader-intake-spec-zero.dxil;out/shader-intake-all/006724-pixel/shader.dxil' -E shaderMain -T ps_6_0 -Fo out/shader-intake-linked-pixel.dxil
```

Outputs are 7,756 bytes (VS) and 5,316 bytes (PS). DXC `-dumpbin` confirms a `shaderMain` definition, stage-model metadata and no remaining `g_SpecConstants` reference in either linked output. Hashes and test assumptions are recorded in `out/shader-intake-all/link-manifest.json`; dumps are `out/shader-intake-linked-{vertex,pixel}.dump.txt`. This requires only DXC's compiler/linker and the two libraries; the native API path uses `IDxcUtils` to create blobs, `IDxcLinker::RegisterLibrary` for each library and `IDxcLinker::Link` with `shaderMain` and the stage profile. No root signature, resource bindings, PSO, draw or frame is established by this link test.

The complete upstream CLI is `XenosRecomp <input path> <output path> <shader common header file path>`. A single-container input writes HLSL; directory input creates the cache. For a full checkout/build, use the exact gitlink above with recursive submodules. Its pinned dependencies are fmt `873670ba3f9e7bc77ec2c1c94b04f1f8bef77e9f`, xxHash `2bf8313b934633b2a5b7e8fd239645b85e10c852`, zstd `f7a8bb1263448e5028aceeba606a08fe3809550f`, smol-v `9dd54c379ac29fa148cb1b829bb939ba7381d8f4`, and renderbag/dxc-bin `737ac9f51c3d06f0cd1ee7e14dc065bafd52310d`. CMake links `fmt::fmt`, `xxHash::xxhash`, `libzstd_static`, `Microsoft::DirectXShaderCompiler`, and on Windows with DXIL enabled `Microsoft::DXIL`; smol-v source is compiled into the executable. These full dependencies were not installed for this probe.

## Current native integration

The probe has been promoted into tracked preparation tooling and original
CreateVS/PS hooks. The same original game entry now acquires all68 native shader
resources and executes original device AddRef. See [native shader creation](native-shaders.md)
for exact commands, ownership, same-run evidence and remaining state/binding limits.
The original first-request checkpoint below is historical evidence for the mapping.

Next dependencies are original effect initialization, real render-state-based
specialization linking, constant and resource bindings, native pipeline/draw/Present,
and title-to-menu input. Additional assets may contain other genuine shader
containers. Successful conversion and resource creation do not establish shader
semantics or correct original frames.

## Original runtime now requests the first converted shader

`out/original-effect-entry-final-boot.log` follows the same original entry through actual file open/query/read/close and original FXOBJ parsing. It reaches CreateVertexShader `0x824ED770` with guestcontainer `0x40019B18`, flags102A1101, virtual bytes178, physical bytesDC, total596. The runtimecontainer SHA-1 `0aa2cf2beb066de65d5cb9bb9b26bb1110487192` matches actualasset `0x1124..0x1378`; its SHA-256 `3845a7a1541bd1b6c7642b934b1be2f664fc0619e0ddfc4ed09a748106b5cb15` matches the first successful `out/shader-intake-all/manifest.json` record (specmask0, DXIL-librarySHA256ac49c69ee0c8ca1e1711e60978f1dc9d42206ef3dd12e547411e840e7ec29482). This links a real game request to the earlier conversion evidence. At that historical checkpoint native shader creation was unimplemented and the original library stopped at824D1858 inside824ED770. The current native adapter now consumes the verified original containers and retains their native stage/library resources. The library artifact is not itself a completed native pipeline or game frame.
