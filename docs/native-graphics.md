# Native Windows graphics device

This document records the owner-only checkpoint `b675db8`. See [native presentation](native-presentation.md) for the subsequent original-game CreateDevice and Clear integration.

The diagnostic now initializes an actual D3D12 device and direct command queue when the original Free Riders flow reaches its observed D3D creation boundary, `0x824F4CF0`. It records the actual adapter description. This increment owns the native renderer foundation; it does not replace guest CreateDevice, create a swapchain, submit game draws, or present a title frame. Original execution still stops at `VdInitializeEngines` after 1,271 function entries.

## Backend and ownership

The backend is [Plume 11926860e878e68626ea99ec88562ce2b8badc4f](https://github.com/renderbag/plume/tree/11926860e878e68626ea99ec88562ce2b8badc4f), exactly the Unleashed reference's graphics gitlink. Bootstrap pins and verifies its four submodules. Source and license remain under ignored `tools/Plume`; CMake links this code, rather than copying a different game's renderer implementation or addresses. The current Windows build uses the installed SDK and D3D12 runtime, with Agility disabled. Plume's Vulkan source is built as part of its upstream static library; this increment selects only its D3D12 interface.

`NativeGraphics` owns the interface, device and direct queue in dependency order and destroys them in reverse. Initialization prepares all objects before publishing ownership. Repeated initialization preserves the same device and queue. Access before initialization throws. The pinned Plume queue factory can return a non-null wrapper after native queue creation fails, so the owner also validates its actual `ID3D12CommandQueue` pointer. Factory/device null failures and invalid native queues throw rather than report initialization success. Driver-failure injection has not been performed.

The owner exposes the real Plume device/queue for subsequent resource and draw adapters. Clients must keep command lists and resources alive until GPU completion; the owner does not silently execute, flush or discard guest work. At present, the diagnostic creates the queue but submits no game work to it. No guest allocation, register, output pointer, callback or return status is changed by this initialization.

The exposed queue's failure is checked by this owner. The pinned upstream device constructor also creates an internal timestamp queue and dereferences its handle without a failure check before returning from `createDevice`; that upstream failure path is not converted into an exception by this wrapper. The current successful hardware runs do not test that failure. Spec and quality reviews approved the scoped owner after the exposed-queue check was added.

## Actual verification

The native test creates upload/readback buffers and a command list/fence. It uploads a nonuniform 4,096-byte pattern, submits a GPU buffer copy, waits for that fence and compares every readback byte. It repeats with different contents using the same objects, verifying fresh completion. It also checks initial access rejection, idempotence and the underlying native queue. CTest imposes a 20-second process timeout. This test needs a working Windows D3D12 adapter; it does not silently skip GPU execution.

The standalone host probe successfully used [D3D12CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice) on the enumerated RTX 4090 adapters and queried resource binding tier 3 / shader model 6.6. Those capabilities support the [dynamic resource facilities described by Microsoft](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html); the probe alone does not prove that every game shader or render state is supported.

Same-run original-entry evidence in `out/native-graphics-boot.log`:

```text
GRAPHICS_ENTRY name=sub_824F4CF0 address=0x824f4cf0 lr=0x8249b414 sp=0x70131440 r3=0x0 r4=0x1 r5=0x0 r6=0x0 r7=0xffffffff83e53ca8 r8=0xffffffff83e53ca4
NATIVE_GRAPHICS backend=D3D12 adapter=NVIDIA GeForce RTX 4090
STOP import-function @0x82acbc2c: __imp__VdInitializeEngines
LAST_FUNCTION sub_825011A0 @0x825011a0 LR=0x825011dc calls=1271
```

Rebuild with the README's `scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-subfze` command. Run `ctest --test-dir out/build/host --output-on-failure`, then `out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader`. The diagnostic's exit 3 is the recorded unresolved service, not successful game initialization.

Evidence files: `out/native-graphics-bootstrap.log`, `out/native-graphics-build-final.log`, `out/native-graphics-ctest-final.log` (17/17), `out/native-graphics-tests-final.log` (103 Python tests, no skips), `out/native-graphics-boot-red.log` (old executable lacks backend initialization), `out/graphics-host-probe.log` and `out/native-graphics-boot.log`. The owner's first test RED was a missing-header compilation failure; its subsequent behavioral verification uses actual GPU execution.

## Subsequent game integration

The original owner-only stop and evidence above describe commit `b675db8`. The subsequent [native presentation adapter](native-presentation.md) creates actual game color/depth resources and submits the original game's first clear. Initialization now occurs inside validated native CreateDevice, rather than inside read-only function-entry tracing. The subsequent [native viewport and ANSI adapter](native-viewport.md) passes that viewport and reaches NtCreateFile `0x82ACB58C` for the original basic shader asset. The [asset-open adapter](asset-files.md) subsequently opens that real file and reaches NtQueryInformationFile. No original game draw or Present has been reached.

[The D3D map](graphics-d3d-map.md) identifies the Free Riders creation, state, shader, draw and presentation boundaries, including layout differences from both reference games. [The shader investigation](graphics-shader-intake.md) converted all 68 validated containers from the original basic-shader asset, and linked one original stage pair using a documented test specialization. These are independent preparation results; no shader is bound to a game draw yet. Root separately verified all 68 source/HLSL/DXIL hash triples and both linked artifacts in `out/graphics-shader-artifact-verification.log`.

The next adapter must provide the verified guest device/resource layouts, route actual state and resource updates into the backend, derive shader specialization from game state, and present the original game's frame. The full title-to-menu acceptance remains unachieved until the same real execution also accepts keyboard or controller confirmation into the original menu.
