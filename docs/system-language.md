# Original user-language query

The native entry now passes the original `ExGetXConfigSetting` call at
`82ACB5FC`, returning to `824D0B84`. Windows `GetUserDefaultUILanguage` is
snapshotted once; this machine returned `0404`, mapped to Xbox language `8`
(traditional Chinese). The original game code remains responsible for choosing
among its supported asset languages. No locale or game branch is patched.

## ABI and effects

Only the audited user category `3`, language setting `9` is implemented.
Category, setting and capacity consume their low 16 bits; guest pointers consume
their low 32 bits. Language output is big-endian 32-bit; the optional required
size output is **big-endian 16-bit**, matching the original stack cell.

| Buffer / capacity | Status | Language write | Required size |
| --- | --- | --- | --- |
| Non-null / at least 4 | `00000000` | Four bytes | 4 |
| Non-null / below 4 | `C0000023` | None | 0 |
| Null / zero | `00000000` | None | 4 |
| Null / nonzero | `C00000F1` | None | 0 |

All effectful ranges are preflighted before any write. The language store
precedes the required-size store, including overlapping outputs. Unused buffers
are not probed. Unknown queries stop explicitly; unrelated reference-emulator
placeholder settings are not supplied.

Mapping preserves Xbox IDs 1 through 12. Chinese Taiwan/Hong Kong/Macau map to
traditional Chinese; China/Singapore map to simplified Chinese. Unsupported,
neutral and custom language IDs stop explicitly. This Windows adapter does not
yet implement other platform locale sources.

## Real execution and limits

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-eqv
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

`out/system-language-boot.log` records the actual native language and successful
query. The unmodified caller then invokes `XGetGameRegion` at `82ACB24C`, return
address `8222AF00`, where execution stops because region semantics are not yet
implemented. Xbox language 8 does not establish which console region to return.
The XEX allowed-region mask is also distinct from a console's runtime region.

`out/system-language-red.log` proves both new tests failed against their initial
stubs; `out/system-language-green.log` records passing implementations. Tests
cover all 12 IDs, regional variants, native Windows provenance, short/null
buffers, exact write widths, overlaps, memory guards and failure atomicity.
The full-suite evidence is in `out/system-language-ctest.log` and
`out/system-language-python.log`: all 44 native tests and 178 Python tests passed,
with no skipped Python tests. An independent code review found no concrete
issues in the query effects, import ABI, language mapping or build integration.

There is still no actual game Draw/Present, title screen or input-to-menu proof.
Shader linking, native resource consumption and PSO integration remain required.

## Sources

- [Windows GetUserDefaultUILanguage](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getuserdefaultuilanguage)
- [Windows language identifiers](https://learn.microsoft.com/en-us/windows/win32/intl/language-identifiers)
- [Pinned Xenia XConfig ABI](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_xconfig.cc)
- [Pinned Xbox language enumeration](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/xbox.h)
- Local original wrapper audit: `out/xconfig-next-audit.md/json`; original
  `824D0B58..824D0BE4` bytes SHA-256
  `ff94648f1f3b61970a56bfec3d3f01f9492287333dc97d6c65552f8fea0df600`.
