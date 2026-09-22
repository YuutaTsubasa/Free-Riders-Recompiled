# Native CreateDevice and Clear: bounded guest contract

Verified 2026-09-09 from ignored local PPC output and the loaded image. This is a static boundary contract, not evidence of successful rendering. References are guest addresses; generated-source line numbers below refer to `out/recomp/ppc/`, not the diagnostic generation. No game instructions, resource payloads, or shaders are included here.

## Observed presentation request

`out/graphics-parameters-boot.log:329` records 31 big-endian words at `0x83E53CA8` (124 bytes). The observed CreateDevice call uses mode r4=1 and flags r6=0.

| Byte offset | Observed value | Verified use |
|---|---|---|
| `0x00 / 0x04` | `0x500 / 0x2D0` | Width / height: 1280 × 720 |
| `0x08` | `0x18280186` | Initial color-surface format |
| `0x0C` | `1` | Retain; field meaning not established here |
| `0x10` | `0` | Surface multisample argument |
| `0x18` | `1` | Retain; field meaning not established here |
| `0x24` | `1` | Enables creation of initial depth surface |
| `0x28` | `0x1A220197` | Initial depth-surface format |
| `0x34` | `1` | Retain; field meaning not established here |
| `0x40` | `0x28280106` | Presentation texture format |

Every other word is zero. In particular, `+0x38=0` permits initial color-surface creation and `+0x3C=0` permits presentation-texture creation. `0x82500DA0` creates these three resources and copies the complete block to device `+0x35BC`; its format normalization can subsequently rewrite copied word `+0x40`, but the observed format stays `0x28280106`.

The locally pinned Marathon `gpu/video.h:141-154` names these formats A8R8G8B8, D24FS8, and LE_X8R8G8B8 respectively. D24FS8 must not silently be identified as ordinary D24S8. These names support interpreting the constants; Marathon's C++ resource layout is not the original Free Riders guest layout.

## Clear entry points

| Entry | Arguments |
|---|---|
| `0x824F6DC8` | r3=device, r4=rectangle count, r5=rectangle array, r6=flags, r7=packed ARGB, f1=depth, r9=stencil |
| `0x824F6CA0` | r3=device, r4=flags, r5=optional single RECT, r6=optional four-float color, f1=depth, r8=stencil |
| `0x824F6620` | Same as the single-rectangle wrapper, with a concrete RECT |

The public wrapper is present in raw `ppc_recomp.78.cpp:15073`, even where diagnostic generation rejects its VMX instructions. Rectangles are four signed big-endian 32-bit coordinates in left/top/right/bottom order, with 16-byte array stride.

- Null rectangle-array pointer: call the single-rectangle wrapper once with a null RECT, regardless of rectangle count.
- Nonnull pointer and zero count: perform no clear.
- Nonnull pointer and nonzero count: clear each supplied rectangle in order.
- These routines do not establish an HRESULT or other stable return value. r3 is volatile and can be changed by nested helpers; retaining r3 in a native void replacement is not proof that the original returned the device or success.

### Flags and no-op cases

`0x824F6620`, especially `0x824F6BF0`, tests `1 << i` for each bound render target at device `+0x3148 + 4*i`, i=0..3. Therefore `0x01/0x02/0x04/0x08` select RT0/RT1/RT2/RT3. Null selected targets are skipped. `0x824F6080:0x824F611C` tests `0x10` for depth; `0x824F6138` tests `0x20` for stencil and consumes the saved stencil argument. Stencil reference is reduced to its low byte when emitted.

When no depth surface is bound at `+0x3158`, the internal routine removes flags `0xF0` and returns immediately if no flags remain. Depth/stencil are applied with the first existing selected color target and removed before subsequent selected targets. If no color bits are selected, there is a separate depth/stencil-only call. A subtle original edge case follows: when color bits are selected but every selected target is null, there is no depth/stencil-only fallback.

Flags `0x40` and `0x80` alter stencil state when `0x20` is set; their full public meaning is not established here. A native implementation supporting only `0x3F` should reject these rather than silently drop them. Zero flags produce no attachment clear; the original may still perform command/state bookkeeping when a depth surface exists.

### Rectangle bounds

For a null single RECT, `0x824F6CA0` chooses RT0, otherwise the depth surface, and decodes surface word `+0x24`:

`width = ((word >> 18) & 0x3FFF) + 1`, `height = ((word >> 3) & 0x7FFF) + 1`.

It forms `[0,0,width,height]`. This original path assumes at least one of those surfaces exists; it is not safe to dereference a null fallback. With device byte `+0x2ABC` bit `0x10`, or bit `0x20` plus the original binding-match condition, dimensions instead come from device `+0x342C/+0x3430`. A bounded ordinary-mode implementation may require these special modes to be absent.

`0x824F6620:0x824F6678` intersects the RECT with the viewport from device `+0x3218`: each of x, y, width, height is independently truncated toward zero, then right=x+width and bottom=y+height. If BE32(device `+0x2F00`) is nonzero, it also intersects with the signed scissor RECT at `+0x3234`. It returns before command emission when right<=left or bottom<=top. Consequently an empty viewport or disjoint scissor makes the clear a no-op, including a null/full-surface RECT.

### Packed color

`0x824F6DC8` stores the packed word in guest byte order, then performs VMX load/rotate/unpack/store. Accounting for `VectorMaskL`, the resulting guest four-float array is **R,G,B,A**, extracted from packed `0xAARRGGBB`.

The loaded image constants are four copies of float bits `0x47008081` at `0x82001AB0` and `0xC7008081` at `0x82001AA0`. Per channel, unpack produces float bits `0x3F800000 | channel`; the VMX multiply-add scales that value by the positive constant and adds the negative constant. This is the normalization construction for channel/255. Exact floating-point equivalence to a division should not be claimed: the raw generated implementation uses separate float multiply and add, so rounding can differ. A conformance check must choose explicitly whether it targets original VMX behavior or the generated arithmetic.

The inner wrapper's null color pointer uses a zero four-float constant at `0x821AC0E0`. The public packed-color wrapper always passes its own nonnull array.

## Initial guest-visible state

Initial color binding calls `0x824E97F8 -> 0x824E9768`. The latter loads viewport defaults from `0x820015E0` (integer x/y=0, requested width/height=65535, float min/max depth=0/1), and scissor defaults from `0x820015F8` (`[0,0,65535,65535]`). `0x824E96C8 -> 0x824E9460` clamps the viewport to the bound surface and stores:

| Device offset | Ordinary initial value |
|---|---|
| `0x3218..0x322C` | Six BE floats `[0,0,width,height,0,1]` |
| `0x3230` | BE32 zero; setter unconditionally writes zero here |
| `0x3234..0x3240` | Four BE32 coordinates `[0,0,65535,65535]` |
| `0x2F00` | BE32 zero, scissor disabled |

Scissor-enable is independently confirmed by state-table record 50 at `0x82AD0A60 + 50*12`: getter `0x824E74A8`, setter `0x824E96B8`, default zero. That setter directly stores device `+0x2F00`. Storing `[0,0,width,height]` as the initial scissor would give the same initial effective clear bounds but would not reproduce the original guest-visible defaults.

Initial resource pointers are depth at `+0x3ABC`, presentation texture at `+0x3AC0`, color surface at `+0x3AC4`; the initial bindings are color at `+0x3148` and depth at `+0x3158`. Presentation texture and color surface are distinct original guest resources.

## Smallest supported surface header

`0x824F3FC0` allocates 48 bytes and calls `0x824F39B0`. Confirmed fields are below. These support a host-owned guest handle only when original allocation/destruction and GPU-command paths are intercepted; they do not make an otherwise opaque handle safe for arbitrary guest resource APIs.

| Surface offset | Observed construction / consumption |
|---|---|
| `+0x00` | Starts at type word 4; creator ORs `0x00100000`, then successful ordinary eDRAM allocation ORs `0x80000000`, yielding `0x80100004` on that original path |
| `+0x04` | BE32 reference count initialized to 1 |
| `+0x14` | BE32 `0xFFFF0000` |
| `+0x18` | Packed aligned pitch/dimensions and multisample mode; MSAA is bits 16..17, not the low bits of `+0x24` |
| `+0x1C` | Packed format/placement; original target setters read it |
| `+0x20` | Depth hierarchy metadata on depth paths; full semantics outside this boundary |
| `+0x24` | `((width-1)<<18) | ((height-1)<<3) | previousLow3`; low three bits are preserved from allocated memory, not explicitly initialized by this routine |
| `+0x28` | Complete guest format word |
| `+0x2C` | Guest allocation size in bytes, computed as tile count × 5120 |

For observed no-MSAA formats and 1280×720, `0x8244ADB8` computes 720 tiles and `+0x2C=0x00384000` (3,686,400 bytes); packed dimensions excluding low three bits are `0x13FC1678`. The size formula rounds width to a multiple of 80 and height to a multiple of 16, uses four bytes per sample for both observed formats, then expresses the result in 5120-byte tiles. It is not generally width×height×4.

Reference-count consumption is independently visible in `0x824F1F50`: atomic BE32 decrement at resource `+4`, then a type/flags-dependent release path when it reaches zero. Thus the first eight bytes are not an invented host vtable. Do not copy original eDRAM ownership flags onto a host handle and permit the original release path to run. Native lifetime handling must prevent release from following original GPU allocation metadata.

Unknown header bytes, the preserved low-three-bit value, and the presentation texture's complete original header are not established by this pass. Guard unsupported guest accesses or implement the relevant API before expanding the mapped layout. A native Clear interception that reads owned attachment metadata avoids depending on the original packed headers, but does not remove the need to satisfy other real guest reads.
