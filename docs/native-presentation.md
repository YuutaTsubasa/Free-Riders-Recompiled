# Original game device creation and first native clear

The title/menu objective is still incomplete. This change replaces two verified Xbox D3D library entry points with real Windows graphics operations. It does not replace the original game entry, caller, assets, or game state transitions.

This document records the CreateDevice/clear checkpoint `62a1b20`. The subsequent [viewport and ANSI checkpoint](native-viewport.md) passes its viewport stop and reaches NtCreateFile. Historical logs and limits below remain applicable to the scope they tested.

## Observed original call

`out/native-presentation-boot.log` records the original `0x824D22F0` entry calling `0x824F4CF0` from LR `0x8249B414`. Its arguments are adapter 0, mode 1, reserved 0, flags 0, parameters `0x83E53CA8`, output `0x83E53CA4`. The 124-byte presentation block contains 1280×720, color `0x18280186`, depth `0x1A220197`, and presentation texture format `0x28280106`. The supported profile is checked completely before native initialization; other modes or formats stop explicitly.

Native creation allocates a D3D12 device/queue, an initially hidden fixed-size HWND, a three-buffer BGRA8 swap chain, a BGRA8 color target, D32_FLOAT_S8_UINT depth/stencil target, framebuffer, command list, and fence. Native handles are checked, including wrappers that upstream factories may return after a native failure. The initial path submits commands and waits synchronously.

`PPC_FUNC(sub_824F4CF0)` and `PPC_FUNC(sub_824F6DC8)` are strong C++ definitions overriding the generated weak public symbols. Original callers and the indirect mapping therefore reach the same adapters without editing generated code. The game returns from CreateDevice and invokes packed Clear with flags `0x3F`, ARGB 0, depth 0, stencil 0. `NATIVE_CLEAR ... submitted=1` is logged after actual queue submission and fence completion. This is a real clear operation, not evidence of a rendered game frame.

The stop observed at this checkpoint was `native-graphics-function @0x824E96C8`, LR `0x8249B288`, after 1,251 function entries. The earlier count was 1,271 at `VdInitializeEngines`; the count decreases because native CreateDevice replaces Xbox driver initialization. Neither driver success stubs nor the original hardware command path are used by the replacement.

## Native verification details

The initial GPU tests exposed two concrete defects: Windows' default minimum captioned-window size changed tiny test dimensions, and pinned Plume's `copyTextureRegion` dereferenced `dstLocation.texture` even for a buffer destination. The window procedure supplies its own minimum-size constraint. The Windows color readback uses native `ID3D12GraphicsCommandList::CopyTextureRegion` with a correctly aligned BGRA footprint, preserving the same resource transitions, queue submission, and fence. No vendor source or dependency pin is changed.

Depth and stencil readback use separate native subresources and footprints queried from the actual D3D12 resource. Their per-pixel data must agree with the submitted clear, independently of the color readback. These are synthetic GPU tests, not game screenshots. The swap-chain creation and clear path are tested; original-game Present has not run.

Failure-path review also identified an upstream swap-chain destructor assuming `textureCount == textures.size()` even when construction returned early. The owner must make that invalid wrapper safe to destroy, check every backbuffer's native handle before publication, and close the DXGI frame-latency waitable handle. Upstream `MakeWindowAssociation` failure can still leak a local COM pointer before it is published to the wrapper; this constructor-internal failure is outside the owner's recovery path. Driver-failure injection and full upstream allocation-failure recovery have not been verified.

## Guest interface and bounds

The guest device has verified size `0x5F00` at `0x71600000`. Render/sampler dispatch addresses and metadata are read from the original image tables; default values are retained by the host adapter for future state translation. Only verified device fields are populated. The implementation has not yet translated those defaults into a draw pipeline.

Stable handles at `0x71700000`, `0x71701000`, and `0x71702000` identify native color, depth/stencil, and presentation objects. Each maps only eight logical bytes with the original reference-count slot. The resource header word is guarded by a throwing read provider; remaining Xbox packed metadata is not invented. Original functions receiving an owned handle in r3 stop before their bodies execute unless that exact entry has a native implementation. This intentionally conservative guard may require later refinement for harmless original wrappers.

Native CreateDevice publishes the guest output only after resources and guest backing exist. A late host allocation failure terminates the diagnostic; rollback/retry of partially mapped fixed addresses is not implemented. This is not a general reusable Xbox D3D runtime yet.

Clear translates packed ARGB to native RGBA, preserves the original null rectangle/zero-count behavior, intersects signed rectangles with the original viewport and optional scissor, and handles selected unbound targets as documented in [the original clear contract](native-clear-contract.md). Rectangles and supported state are checked before GPU submission. Only the native primary render target and depth target are currently supported. Special clear modes, flags beyond `0x3F`, other targets, unknown resource fields, and unrelated graphics entries stop explicitly.

## Viewport dependency at this checkpoint

Raw `ppc_recomp.77.cpp` establishes `0x824E96C8` as an integer-viewport wrapper: r3 is device, r4 points to four unsigned32 x/y/width/height values followed by two float32 depth bounds. It converts each integer to float and calls `0x824E9460`. That implementation clamps dimensions against current attachments, writes the seven-word viewport at device `+0x3218`, updates scissor via `0x824E8C70`, updates viewport scale/offset values at `+0x2908..0x291C`, and marks dirty state. These are static source observations; the next adapter must validate actual input values and preserve their effects in the native draw path.

The rebuilt original-entry run `out/native-presentation-boot-final.log` records r4 `0x70131500` with six BE words `0,0,500,2D0,3F800000,0`: x/y 0, width 1280, height 720, minimum depth 1.0, maximum depth 0.0. The next native viewport adapter must preserve this observed reversed depth mapping rather than assume the usual 0→1 interval.

Shader creation/cache, textures, render state, actual draws, original Present, input, and later initialization services remain necessary. The HWND stays hidden until an actual native Present; clearing a texture does not satisfy title/menu acceptance.

## Reproduction

Build with `./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-subfze`, then run `./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader`. Exit 3 denotes the explicit unresolved graphics function, not success. All image, asset, generated source, executable, and test-output files remain local and ignored by Git.

Final build and verification logs are `out/native-presentation-build-final.log`, `out/native-presentation-ctest-final.log` (19/19 native tests), `out/native-presentation-tests-final.log` (103 Python tests with actual image/native executable enabled, no skips), and `out/native-presentation-dependencies.log` (both pinned repositories verified). The initial failing native tests remain in `out/native-presentation-gpu-red.log`. A final focused boot regression also verifies the newly recorded reversed viewport request.
