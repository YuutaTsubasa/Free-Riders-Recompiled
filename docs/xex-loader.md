# Executable module metadata contract

The local Free Riders `sub_824DCF70` reads the import slot at `0x82000778`, dereferences it again, loads offset `0x58` from the resulting module, and calls import `RtlImageXexHeaderField` at `0x82ACB67C` with key `0x20401`. The verified source header contains 16 optional entries and does not contain this key. Returning zero for that lookup reflects the source metadata.

The runtime exposes a guest handle variable at `0x71000000`, a minimal module at `0x71000040` and the original XEX header at `0x71010000`. These are host-chosen diagnostic allocations outside the loaded image, stack and PCR. Only the handle word and observed module field are accessible; the remaining module fields stop on access. This is not a complete Xbox module loader.

`sfr_image_dump` exports the original first `0x5000` bytes as `xex_header.bin` before publishing `complete.txt`. The diagnostic requires the fixed size and SHA-1 `82ef42d3f130b26563fcd85909f2eab613de6c68`, as well as the existing decoded-image and import-table fingerprints, before executing guest code. Old image dumps must be regenerated into a new directory.

Lookup validates the supplied header pointer against the loaded header. Optional entries use the low byte of their key:

| Low byte | Return value |
| --- | --- |
| 0 | Immediate value stored in the entry |
| 1 | Guest pointer to the inline value word |
| 2 through 254 | Guest pointer to fixed-size data at the entry's offset |
| 255 | Guest pointer to variable-size data including its size word |
| Key absent | Zero |

The constructor validates magic, table count and bounds, duplicate keys and complete payload ranges before mapping data. It caches validated lookup results; a guest write to directory offsets cannot redirect the host lookup to an unchecked pointer. Guest payload memory remains subject to the ordinary diagnostic memory checks.

The import dispatcher implements only the exact verified name/address pair for this lookup. The result is zero-extended into guest r3; r4 and the other register state are preserved. Memory allocation is described separately in [virtual-memory.md](virtual-memory.md); unimplemented imports retain their named stops.

References consulted at Xenia revision `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`:

- [UserModule::GetOptHeader](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/user_module.cc): runtime immediate versus pointer semantics.
- [X_LDR_DATA_TABLE_ENTRY](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xmodule.h): XEX header field at offset 0x58.
- [RtlImageXexHeaderField](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc): returning the lookup value to the caller.

The build's pinned XenonUtils `getOptHeaderPtr` is a host parser API and returns a pointer even for kind zero; using that behavior for the Xbox runtime would be incorrect.

## Executable privilege query

`XexCheckExecutablePrivilege` at the exact import pair `__imp__XexCheckExecutablePrivilege`/`0x82ACB5AC` accepts a bit index in r3 and returns Boolean0/1, zero-extended into r3. It queries the cached immediate `XEX_HEADER_SYSTEM_FLAGS` entry (key `0x00030000`), with an absent entry equivalent to flags0. This follows pinned Xenia [xboxkrnl_modules.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc).

Indexes0–31 are supported using an unsigned32-bit mask; indexes32 and above produce a diagnostic stop before any shift. The out-of-range policy is diagnostic containment, not a claim about Xbox error semantics. Guest changes to mapped header bytes cannot change the original cached system flags. A kind1 key `0x00030001` is a different key and cannot be used as the flags value.

This title's verified header has flags `0x220`: bits5 and9 set, bit10 clear. The observed `sub_824D2108` caller queries privilege10 with return address `0x824D212C`, so its result is0. Both reference recompilations return a constant0 for this function; the diagnostic instead derives the answer from the source metadata. Synthetic tests include positive answers, bit31, absence, invalid indexes and header mutation.
