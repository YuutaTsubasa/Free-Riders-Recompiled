# Native system module identity

This page preserves the initial identity checkpoint. Current bounded missing-export lookup and resident unload behavior is documented in [party-export policy](party-export-policy.md); current boot has advanced to 1,030 entries.

The Windows runtime owns a bounded XAM service namespace. `XexLoadImage` can acquire its stable opaque identity; this does not load or execute an Xbox `xam.xex` binary, and does not imply its exports are implemented. Original game instructions still perform the request and consume its result.

## Supported contract

Only the exact import pair `__imp__XexLoadImage` / `0x82ACB71C` is dispatched. Arguments are r3 checked guest C string, r4 flags, r5 minimum version and r6 output word. This checkpoint accepts only `xam.xex`, flags 9 and minimum version 0. Other requests stop explicitly, without pretending a missing game file exists or returning a guessed status.

The host record starts with zero acquisitions and returns the same handle `0x71500000` each time. The count increments only after the complete output word has passed write checks and been stored in big endian order. Acquisitions stop at 65535. The handle is a guarded guest word: direct structure access stops instead of exposing a fabricated Xbox loader structure. The count is host-owned, not a guest LDR load-count field. Unload and procedure lookup remain unsupported.

Names have a 260-byte limit including NUL and use checked reads with 64-bit address arithmetic. Null pointers, invalid strings, invalid output ranges, import guards, read-only outputs and reservation interference stop before any output/count mutation. Capturing the name before storing the handle permits a source/output alias. The runtime returns zero in r3 only after acquiring the actual host record and writing its identity; other registers remain unchanged.

## Evidence and limits

Pinned Xenia [XexLoadImage](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc#L86) checks existing modules before attempting a user image load, increments a load count, writes the module handle and returns status. Its complete loader, kernel object model and guest LDR layout are broader than this native record. The inspected Marathon procedure/section lookup hooks are stubs and provide no behavioral evidence to copy.

Original caller `sub_82A52C60` passes the string at `0x82000910` through `sub_824D1E50`; the latter supplies flags 9, version 0 and a stack output at `0x7013FBD0`. After success the original caller requests ordinal 2815 (`0xAFF`), followed statically by 2816, 2827 and 2832. Xenia's pinned [XAM export table](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_table.inc#L1683) identifies these as `XamPartyGetUserList`, `XamPartySendGameInvites`, `XamPartySetCustomData` and `XamPartyGetBandwidth`. Names and static call sites establish neither execution of all four nor their ABI. Unknown procedure lookup must still stop until its contract is implemented.

This is an initialization dependency for the [genuine title/menu goal](title-menu-goal.md). It provides no game rendering, keyboard/controller input or playable menu.

The procedure wrapper `sub_824D1DE0` also reads the returned module handle plus `0x18` after a successful procedure lookup, comparing it with the resolved function address. That offset corresponds to `dll_base` in Xenia's LDR structure. The current opaque record deliberately does not supply that field: future export support must establish the corresponding native module/address contract. A zero-filled larger mapping would conceal this dependency.

## Module-identity checkpoint result

`out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader` now records:

```text
IMPORT XexLoadImage module=xam.xex flags=0x9 min_version=0x0 output=0x7013fbd0 handle=0x71500000 load_count=1 status=0x0
IMPORT_CONTEXT name=__imp__XexGetProcedureAddress address=0x82acb70c lr=0x824d1dfc sp=0x7013fb70 r3=0x71500000 r4=0xaff r5=0x7013fbc0 r6=0x7013fbd0 r7=0xffff r8=0xf20a r9=0xf209 r10=0xffffffff82000000
STOP import-function @0x82acb70c: __imp__XexGetProcedureAddress
LAST_FUNCTION sub_824D1DE0 @0x824d1de0 LR=0x824d1dfc calls=524
```

Exit status is 3. The original caller consumes the handle and queries its first export; no procedure pointer has been returned and none of the four party exports has executed. There are now 15 bounded implemented imports and three separately counted Etx unavailable policies.

Local ignored evidence: `out/native-modules-boot-red.log` (new boot assertion fails on the preceding binary), `out/native-modules-build.log`, `out/native-modules-boot.log`, `out/native-modules-tests-final.log` (94 tests, no skips), `out/native-modules-ctest-final.log` (12/12), plus pinned dependency verification. The component's initial RED was a missing-source compile failure; it is not behavioral TDD evidence.

A later mutation check compiled the existing component tests against a temporary no-op `load_image` with the real guarded constructor. It failed on the missing big-endian handle write (exit 1); the real component passed (exit 0). This verifies rejection of a compiled no-op, without retroactively claiming behavioral TDD.

Subsequent [party-export failure handling](party-export-policy.md) adds bounded failed lookup for six explicitly absent native exports, exact status conversion and resident kernel-module unload. The earlier statements that all procedure lookup/unload is unsupported describe this identity checkpoint. The current boot advances to 1,030 entries and stops at an untranslated `subfze`; no callable party export or LDR field has been added.
