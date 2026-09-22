# Original viewport and counted shader filename

The original Windows game now passes its viewport call and constructs the ANSI filename descriptor for `game:\shader\Xbox360BasicShader.fxobj`. The title/menu objective remains incomplete: no original game draw, Present, or confirmation input has run.

This document records checkpoint `cf05e15`. The subsequent [asset-open adapter](asset-files.md) passes NtCreateFile and reaches the original file-information query. The logs below retain the earlier viewport/ANSI evidence.

## Original execution evidence

`out/native-viewport-ansi-boot.log` starts at the verified original `0x824D22F0` entry and records actual D3D12 creation, clear, and:

```text
NATIVE_VIEWPORT_REQUEST address=0x70131500 words=0,0,500,2d0,3f800000,0
NATIVE_VIEWPORT source=0x824e96c8 x=0 y=0 width=1280 height=720 min_depth=1 max_depth=0 updated=1
IMPORT RtlInitAnsiString destination=0x70131318 source=0x821a8fe4 length=37 maximum_length=38
IMPORT_CONTEXT name=__imp__NtCreateFile address=0x82acb58c lr=0x824d108c sp=0x701312b0 r3=0x70131310 r4=0xffffffff80100080 r5=0x70131328 r6=0x70131320 r7=0x0 r8=0x0 r9=0x0 r10=0x1
STOP import-function @0x82acb58c: __imp__NtCreateFile
LAST_FUNCTION __savegprlr_26 @0x82a56060 LR=0x824d108c calls=1265
```

The filename bytes at image address `0x821A8FE4` identify the original basic shader asset. This execution has prepared its descriptor and reached file creation/opening; it has not yet read the file. The next necessary dependency is a checked native asset file interface preserving the observed Xbox ABI.

## Raster state contract

Strong public hooks implement integer viewport `0x824E96C8`, scissor rectangle `0x824E8C70`, and scissor enable `0x824E96B8`. The guest adapter checks the known device and attachment, converts the original four unsigned coordinates to float, clamps extents to the attachment, preserves the two depth values, and writes the original viewport, scale/offset, raw and packed scissor fields and dirty mask `0x07E00000`. Checked destination ranges and valid native state are required before guest mutation. Missing attachments preserve the original no-op behavior. Special modes, unknown attachments, signed coordinate overflow and unsupported graphics remain explicit stops.

NativePresentation retains NativeRasterState and applies actual Plume viewport/scissor commands to native command lists, including subsequent clear work. GPU work uses the existing queue and fence. Inverted depth is accepted only when the actual device reports `InvertedViewportDepthFlipsZSupported` through [D3D12_OPTIONS13](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options13). Its depth mapping follows the [DirectX specification](https://microsoft.github.io/DirectX-Specs/d3d/VulkanOn12.html#inverted-viewports). Unsupported devices stop on the game's observed 1→0 request; capability is not assumed from the adapter name.

The original no-argument getter `0x82211440` is exempted from the conservative r3 native-handle guard after auditing both original branches: each overwrites r3 before reading it. This allows the unchanged getter to execute when a preceding void graphics call leaves a handle in volatile r3. No other unimplemented graphics function is made callable by this exception.

The offscreen native test executes an actual test-only shader and reads every color/depth pixel. It verifies coverage from viewport intersected with scissor and depth 0.25 with 0→1 versus 0.75 with 1→0 on a capable device. A new command list verifies retained state. Synthetic shader code exists only in the test and does not represent a game frame. Guest tests also check original fields, optional scissor, actual clear clipping, empty extents, unsupported reversed depth, and atomic validation failures.

## ANSI_STRING contract

The exact `RtlInitAnsiString` import `0x82ACB57C` preserves its void ABI and PPC registers. A checked helper writes eight guest bytes: big-endian uint16 length, uint16 maximum length and uint32 original source pointer. A null source gives 0/0/0; a non-null empty source gives 0/1/source. The pointer aliases original guest data without allocation or copying.

The complete destination is checked before source access. Scanning uses checked byte loads and 64-bit address arithmetic. Lengths through 65,534 are supported; no NUL within 65,535 checked bytes, invalid memory, read-only destination or a 4 GiB crossing stops before any descriptor mutation. Oversize wrap/clamp semantics differ among references and are intentionally unsupported.

ABI evidence is pinned Xenia `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`, `xboxkrnl_rtl.cc` initializer and `xbox.h` X_ANSI_STRING. The reference recompilations agree on normal non-null descriptors; Xenia's explicit null contract is used. Tests cover exact bytes, high-bit characters, source preservation, null/empty distinction, last mapped NUL, maximum length and rejection atomicity.

## Verification and reproduction

Run from the project root:

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-subfze
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader
ctest --test-dir out/build/host --output-on-failure
```

The executable currently exits 3 at NtCreateFile; this is a recorded missing service, not game completion. Python environment setup remains in README. All ROM, assets, generated files and logs remain local and Git-ignored.

Final evidence: `out/native-viewport-ansi-build.log`, `out/native-viewport-ansi-ctest-final.log` (21/21), `out/native-viewport-ansi-tests-final.log` (103 Python, no skips), and `out/native-viewport-ansi-dependencies.log` (both pins verified). Separate specification and quality reviews approved native raster, guest integration and ANSI helper.

Expected RED evidence includes `out/native-viewport-boot-red.log`, `out/native-viewport-guest-red.log`, `out/native-viewport-raster-red.log`, `out/ansi-string-test-red.log`, and `out/ansi-string-boot-red.log`. Review caught signed endpoint overflow; `out/native-viewport-overflow-red.log` reproduces it and `out/native-viewport-overflow-green.log` confirms the fix. GPU testing also caught a readback harness error: footprint base offsets must be aligned separately and buffer sizing must include that offset, without requiring padding after the final row. The corrected actual GPU test passes in the final full suite. No vendor pin or source was changed.
