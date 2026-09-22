# Free Riders native graphics boundary investigation

Investigation date: 2026-09-09. This is an evidence map, not an implemented graphics backend. No game code, assets, command streams, or shader binaries are reproduced here. Addresses are guest virtual addresses. Generated-source references below are local, ignored files under `out/recomp/diagnostic-subfze/`; line numbers describe that diagnostic generation.

## Evidence and confidence

Static facts come from the generated PPC instruction semantics. The names CreateDevice, Present, viewport, and other API roles are inferred from data flow and comparison with these locally verified reference commits:

- MarathonRecomp `bd9c0bbd8a99bcc2c0fabdf9521462e75e0ae7d8`: `MarathonRecomp/gpu/video.cpp:2323,7978`; `video.h:47`. Its native CreateDevice hook replaces `0x8253EC98`, takes six arguments, and allocates a `0x5000` guest device.
- UnleashedRecomp `cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c`: `UnleashedRecomp/gpu/video.cpp:2095,7798`; `video.h:34`. Its equivalent hook replaces `0x82BD99B0`, takes six arguments, and allocates a `0x5E00` device. Resource, state, draw, and Present hooks follow at `video.cpp:7800` onward.

The root investigation separately traced the original game entry to `0x824F4CF0`, return address `0x8249B414`, with low-32-bit arguments `0,1,0,0,0x83E53CA8,0x83E53CA4`. Its evidence is `out/graphics-boundary-boot.log`. This bounded investigation did not execute a completed device creation or observe a presented frame. The observed boot still stops at `VdInitializeEngines` after 1,271 guest-function calls.

## Device creation boundary

`0x824F4CF0` is a strong CreateDevice match (`ppc_recomp.78.cpp:9449`). Its six-register ABI is:

| Register | Confirmed use | Inferred API meaning |
|---|---|---|
| r3 | Not consumed before being overwritten | Adapter argument |
| r4 | Saved; value 2 selects the alternative initialization path | Device type/mode |
| r5 | Not consumed | Reserved/window argument; exact name unverified |
| r6 | Saved, normalized, written to device `+0x5E88` | Behavior flags |
| r7 | Passed to initialization; later read and copied as a 124-byte block | Presentation parameters |
| r8 | Output address is cleared first; receives the created guest pointer on success | Big-endian 32-bit device output |

The routine requests `0x5F00` zeroed bytes with alignment argument `128` from `0x824F4B28`. That helper calls `0x824F4AD0` and clears the requested byte count. Creation returns zero after publishing the output pointer; allocation/initialization failure paths return `0x8007000E`. On the normal path, absent flag `0x100` and absent any `0x3F000000` bits, creation adds `0x0C000000` to the supplied flags.

Confirmed initialization chain:

1. `0x82501238` establishes initial fields and requests a 4,800-byte allocation (`ppc_recomp.79.cpp:5197`).
2. For mode other than 2, `0x825012A8` initializes critical sections and calls `0x825011A0` (`ppc_recomp.79.cpp:5269`).
3. `0x825011A0` calls `VdInitializeEngines` at `0x825011D8`, then registers the graphics interrupt callback (`ppc_recomp.79.cpp:5106`).
4. Further normal initialization allocates command/state data, creates presentation resources through `0x82500DA0`, and populates default state through `0x82500B38`.

Mode 2 instead calls `0x824F7240` and `0x82501510`; two statically found callers, `0x8278FB88` and `0x82792A08`, pass null presentation parameters on this path (`ppc_recomp.117.cpp:404,4777`). A replacement must preserve that distinction rather than dereference every r7.

The actual game graphics object is based at `0x83E53BE8`: `0x8249B3C0` passes its `+188` field as device output and its `+192` block as presentation parameters, with mode and flags at `+320/+324` (`ppc_recomp.68.cpp:1637`, call at `0x8249B410`).

`0x82500DA0` consumes parameter offsets `+0/+4` as the first two dimensions of resource creation, `+8` as a format-like value, `+16` as a multisample-like argument, `+36` as the conditional depth-resource switch, and `+40` as that resource's format-like argument. It copies 124 bytes to device `+0x35BC`. These field roles are strong inferences; this is not a complete validated presentation-parameter declaration.

## Guest device fields and default state

Do not copy either reference's GuestDevice definition wholesale. These are independently observed Free Riders offsets; field names remain inferred unless described as a literal storage operation.

| Offset | Observed behavior / likely role | Evidence |
|---|---|---|
| `0x00..0x27` | Five 64-bit words set to all ones during default GPU-state setup; used as dirty masks | `0x8250B698`, `ppc_recomp.79.cpp:29254` |
| `0x30/0x34/0x38` | Current command cursor/end/guard limit | `0x824F7240`, `ppc_recomp.78.cpp:13662` |
| `0x3C` | Initialized to 1 | `0x82501238` |
| `0x40` | 101 function-pointer entries, index stride 4; render-state dispatch | `0x82500B38`, `ppc_recomp.79.cpp:4138` |
| `0x1D4` | 20 function-pointer entries; sampler-state dispatch | Same routine |
| `0x224 / 0x3B8` | Corresponding render/sampler metadata arrays | Same routine |
| `0x480` | 26 records visited at stride 24 during sampler initialization | Same routine; `0x8250B698` also updates 26 records |
| `0x780 / 0x1780` | Constant-data regions passed with register bases `16384/17408`; likely vertex/pixel float constants | `0x824F4DC8`, `ppc_recomp.78.cpp:9660,9676` |
| `0x2A90 / 0x2A94` | Allocated 96-/32-byte blocks | `0x825012A8` |
| `0x3148 + 4*i` | Surface argument stored by `0x824E97F8`; render target slots | `ppc_recomp.77.cpp:2179` |
| `0x3158` | Surface argument stored by `0x824E9B48`; depth surface | `ppc_recomp.77.cpp:2668` |
| `0x3218` | Seven-word viewport-like structure saved by Present; setter reads six floats plus a seventh word | `0x824E65A0`; `0x824E9748`, `ppc_recomp.77.cpp:2065` |
| `0x3234` | Four-word scissor-like structure saved/restored by Present | `0x824E65A0` and `0x824E8C70` |
| `0x35BC` | 124-byte copy of presentation parameters | `0x82500DA0`, `ppc_recomp.79.cpp:4486` |
| `0x3ABC / 0x3AC0 / 0x3AC4` | Initial depth surface / presentation texture-like resource / color surface | `0x82500DA0`; the latter two are consumed by Present |
| `0x4230` | Pointer to 4,800-byte allocation | `0x82501238` |
| `0x5E88` | Normalized creation flags | `0x824F4CF0` |

`0x82500B38` reads 101 12-byte records from `0x82AD0A60`, placing record `+4` into the render-state function table and record `+0` into metadata, then calling the function with record `+8` as its default value. It similarly reads 20 records at `0x82AD0F20` for sampler state, invoking them across 26 sampler indices. A native adapter can use these tables as a mapping source, but invoking the original setters without a GPU-command strategy still reaches guest command-writing code.

The viewport offset is `0x3218`, whereas Unleashed's declared viewport is `0x3168`; even though the early dispatch-table and float-constant offsets agree, the later layout differs. The seven-word Free Riders viewport boundary also needs its last word validated before treating it as a standard six-field structure.

## Present and resource boundaries

`0x824E65A0` is the strongest public Present candidate (`ppc_recomp.75.cpp:19180`). The game graphics wrapper `0x8249BC40` calls it with the device from object `+188` at `0x8249BC6C` (`ppc_recomp.68.cpp:3000`). A second caller is present in `ppc_recomp.132.cpp:8996`.

It saves viewport/scissor state, selects the initial color surface through `0x824E97F8`, uses the presentation resource at device `+0x3AC0`, restores state, then calls `0x824E5F18` with device, that resource, and device `+0x361C`. Consequently, replacing only the innermost swap import would miss preceding GPU command generation.

`0x824E5F18` (`ppc_recomp.75.cpp:18224`) is the only generated direct caller of `VdSwap`. Its call instruction is `0x824E61D8` (return address `0x824E61DC`); before it, `VdGetSystemCommandBuffer` runs at `0x824E6144`. A separate worker `0x8250B228` also calls this internal routine (`ppc_recomp.79.cpp:28734`). The internal function accepts a resource/descriptor, copies part of its header, writes command data, and updates device swap bookkeeping. It is a lower boundary than public Present, not an interchangeable zero-argument host Present function.

Useful next resource/state candidates with confirmed data flow:

- `0x824F3EA0`: creates the texture-like presentation resource; normal initialization passes dimensions plus additional depth/level/format parameters (`ppc_recomp.78.cpp:7192`). Exact generic creation ABI still needs validation.
- `0x824F3FC0`: surface creator used for both initial color and depth surfaces (`ppc_recomp.78.cpp:7362`).
- `0x824E97F8(device,index,surface)`: render-target setter; writes guest surface pointer to `device+0x3148+4*index` and consumes its packed header.
- `0x824E9B48(device,surface)`: depth setter; writes `device+0x3158` and consumes surface header fields.
- `0x824E9748(device,viewport)`: structure wrapper over `0x824E9460`, forwarding six floats and a seventh word.
- `0x824E8C70(device,rectangle)`: scissor-like setter used by Present to restore four saved words.

## Next minimal native adapter contract

The next meaningful implementation is a real higher-level graphics adapter at the observed game-library boundary. Completing CreateDevice alone would only move the diagnostic stop.

1. Replace `0x824F4CF0` with a validated six-argument bridge backed by an actual host graphics device/swapchain. Allocate an addressable `0x5F00` guest device, honor normal and mode-2 callers, write the big-endian output only after initialization succeeds, and retain failure reporting. Initialize only independently mapped fields and dispatch tables; add observed fields as their callers are verified.
2. Provide owned guest resource handles whose layouts satisfy any guest reads that remain. Implement creation, locking/upload, reference lifetime, render/depth targets, viewport, scissor, sampler/render state, constants, shader binding, and geometry submission for the calls the real title path reaches. A native resource pointer cannot replace a guest pointer unless that mapping is explicitly supported. Unsupported formats/state must remain explicit diagnostic failures.
3. Map translated game shaders and original game resource data to actual host draw calls. State-table setup must route supported selectors into this backend. Native CreateDevice success with null resources or ignored draw calls does not meet the title/menu objective.
4. Hook the verified public Present boundary only once its producer path is implemented; preserve any required guest-visible state and frame bookkeeping. Evaluate the alternate internal worker route separately. A black swapchain or fabricated menu is not evidence of the game rendering.
5. Validate by booting the original entry, recording the real graphics API sequence and resource/shader identities, and presenting the title's own frame. The current evidence establishes promising interception points, not a completed renderer or a title-screen result.

## Shader creation, binding, and cache lookup extension

These are static mappings from a second bounded pass. No shader-creation or draw call below has yet been observed dynamically by this investigation.

| Candidate role | Guest address | Independently observed contract | Generated reference |
|---|---|---|---|
| CreatePixelShader | `0x824ED588` | r3 points to the complete container; returns allocated resource in r3 or zero | `ppc_recomp.77.cpp:10443` |
| CreateVertexShader | `0x824ED770` | Same one-pointer input and resource/null result | `ppc_recomp.77.cpp:10740` |
| SetPixelShader | `0x824EBB00` | r3 device, r4 resource; stores binding at device `+0x3244` | `ppc_recomp.77.cpp:6562` |
| SetVertexShader | `0x824EBD08` | r3 device, r4 resource; stores binding at device `+0x3248` | `ppc_recomp.77.cpp:6864` |
| DrawPrimitive | `0x824F52D0` | r3 device, r4 primitive encoding, r5 start vertex, r6 element count | `ppc_recomp.78.cpp:10369` |
| DrawIndexedPrimitive | `0x824F56E8` | r3 device, r4 primitive encoding, r5 base vertex, r6 start index, r7 element count | `ppc_recomp.78.cpp:10991` |
| DrawPrimitiveUP | `0x824F5288` | r3 device, r4 primitive encoding, r5 element count, r6 input bytes, r7 byte stride | `ppc_recomp.78.cpp:10319` |
| Clear wrapper | `0x824F6CA0` | r3 device, r4 flags, r5 optional rectangle, r6 optional color pointer; forwards f1 and r8 to internal clear | `ppc_recomp.78.cpp:13000` |

The shader stage identifications have substantially more support than proximity to reference addresses: the pixel creator builds a 40-byte wrapper initialized with type value 7 by `0x824EBAC8`, whereas the vertex creator builds an 872-byte wrapper initialized with type 6 by `0x824ED678`. The pixel setter reads its copied container at resource `+40`; the vertex setter reads it at resource `+872`. Their dirty-mask handling uses device `+8` and `+0`, respectively, consistent with the independently mapped pixel and vertex constant regions. The supplied asset's validated containers have stage flags ending in `0` for pixel and `1` for vertex (see `graphics-shader-intake.md`), matching the creation allocation branch.

Both creators read big-endian container words at offsets `+4` and `+8`. The first controls the virtual-section copy length; the second controls a separate physical allocation/copy. Source physical bytes begin at `container + BE32(container+4)`. For the appropriate stage, virtual bytes are copied after the stage-specific wrapper. The allocation calculation is `virtualLength + 40 + ((flags & 1) ? 832 : 0)`. A future native creator can intercept before any original relocation, hash the validated raw container, and associate its returned guest resource with the actual native shader cache entry. Do not hash the relocated wrapper.

The exact reference cache key is **XXH3-64 over raw container bytes of length `BE32(container+4) + BE32(container+8)`**, with checked bounds and overflow before accessing memory. Confirmed sources: Unleashed `gpu/video.cpp:5046-5098`, Marathon `gpu/video.cpp:5565-5613`. Both look up this hash in their converted shader cache. It is neither a hash of the outer `.fxobj` nor a hash of instruction bytes alone. The current intake manifest's SHA-256 is useful for provenance but is a different key; either generate matching XXH3 keys from the same complete containers or deliberately define and use one consistent alternate key on both sides. Cache identity must also validate the expected stage. A miss remains a missing real shader, not permission to return an empty shader object.

The static caller `0x827F648C` loads vertex-container pointers from a table and calls `0x824ED770`; `0x827F64AC` loads the subsequent pixel-container pointers and calls `0x824ED588` (`ppc_recomp.127.cpp:10411,10431`). Additional creation calls exist in `ppc_recomp.58.cpp:17135-17247`, `ppc_recomp.111.cpp:11344-11481`, and `ppc_recomp.128.cpp:16012,16157`. These establish actual game/library use beyond the functions' internal shapes, but do not establish which shader is reached first by the current title boot.

### Draw and clear evidence limits

`0x824F52D0` flushes the five dirty-mask regions, writes the supplied start vertex, and packs chunks of the supplied count into a command word together with the primitive encoding. `0x824F56E8` follows the same state path, fetches the bound index resource from device `+0x3144`, and forms its data address using either `startIndex*2` or `startIndex*4` according to resource bits. Its independent base-vertex parameter goes into the same register-write position occupied by start vertex in the nonindexed path. Both split large counts around the 16-bit command count limit using primitive-specific alignment data.

These are **element counts** at this boundary: do not multiply them again using a conventional desktop D3D primitive-count formula. This agrees with the reference adapters, whose parameters are named `primitiveCount`/`primCount` but are passed directly to host `drawInstanced`/`drawIndexedInstanced` (`Unleashed gpu/video.cpp:4611-4681`). Exact primitive-enumeration translation, instancing behavior, index format interpretation, and full resource layout still need independent implementation/validation.

`0x824F5288` calls `0x824F4DC8` to reserve data space and copies exactly `count*stride` input bytes into it. That establishes the UP data/count/stride mapping without interpreting all command packets. Actual callers include `ppc_recomp.65.cpp:7069,7195` and `ppc_recomp.129.cpp:12666,13194`; indexed callers include `ppc_recomp.49.cpp:3652,4037`, and nonindexed callers include `ppc_recomp.59.cpp:12431,14930`.

For Clear, `0x824F6CA0` builds a full surface rectangle if r5 is zero, otherwise retains the provided rectangle, then calls `0x824F6620` at `0x824F6DB0`. The internal routine clips against viewport/scissor, reads color from r6 (using a default when null), retains f1 as a depth-like float, and retains r8 as a stencil-like word. Its interactions with the bound depth surface and render targets strongly support Clear, but depth/stencil flag semantics and the full calling convention remain to be proved. No generated direct caller of this outer wrapper was found in this pass. In particular, copying Unleashed's treatment of its third argument as unused would discard an actual Free Riders rectangle.

The next useful runtime evidence is the first real create-shader call: record creator address, container address, validated stage/length and full-container cache key, then the resource returned and the corresponding setter call. Converted DXIL libraries alone do not establish that association, specialization state, vertex layout, or a submitted game draw.
