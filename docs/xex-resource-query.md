# Original embedded XEX resources

The original title/menu goal remains **unachieved**. The next dependency after
`6da84b4` is XexGetModuleSection for the original `TEX_00` resource.

## Source and metadata

The verified original XEX's resource header key `000002FF` is at file offset
`330C`. Its length is 356 bytes, including its four-byte size field and 22
sixteen-byte entries. Each entry has eight name bytes, a big-endian address,
and a big-endian size. Names include title `5345084D` and `TEX_20..TEX_00`.

The [Xenia resource lookup](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/user_module.cc)
and [kernel export](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc)
return the resource address/size from this table, leaving outputs unchanged
on absent resources or invalid handles. The pinned `utf8.cc::equal_z` compares
exact characters through the first NUL; an eight-byte resource name need not
contain a terminator. This implementation supports the original ASCII names.

`TEX_00` resolves to `84099E80`, length `1080`, already within the decoded image.
Its 4,224 original bytes begin with DDS magic and have SHA-1
`2aae5962f6b55ed309919dd3d32c7c2867908e8e`, SHA-256
`36dbcfa2315d562c17cd4ece64c418e35924432a44849570bf9dd0e6bf9553c1`.
The independent image audit is `out/xex-resource-original.json`. The data is
not generated, replaced, or allocated by this lookup.

## Checked publication

Metadata is captured from the validated supplied header before module mappings
are published. Guest mutation of mapped header bytes cannot redirect a lookup.
The current loader entry is the only supported identity. Invalid handles return
`C0000008`; a valid but absent resource returns `C0000225`. Invalid pointers and
unsupported name formats remain explicit stops.

The complete name is captured before any output writes, including when it
aliases an output. For a found resource, its complete data range and both
aligned, distinct, writable output words are checked first. Thus a bad second
output cannot leave a partially published result. Lookup allocates no guest
resources and does not change a module load count.

## Original texture wrappers and actual execution

Independent instruction inspection shows `8250E960` forwards arguments to
`8250E188`; neither dereferences the opaque device before the original DDS
parser. Only four exact device/address/LR combinations were admitted:

| Entry | Required return address |
| --- | --- |
| `8250E960` | `8243D1AC` |
| `82A56058` stack save | `8250E968` |
| `8250E188` | `8250E9C4` |
| `82A56030` stack save | `8250E190` |

All require device `71600000`. The stack helpers only save registers relative
to r1. No other device consumer was admitted. Wrapper E960 is 27 instructions,
108 original bytes, SHA-256
`cabf8549014dc08f83652110e946a4a8f531de3198c1b98b819047bdd3e6f3ff`;
E188 is 501 instructions, 2,004 bytes, SHA-256
`550d4a6d9b4a84fcfa73f41907014a3c5f591fc8a0658d886178e48f7f6dd456`.
The byte audit is `out/texture-wrapper-original.json`.

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-doubleword
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

The actual `out/xex-resource-final-boot.log` records exit 3:

```text
RESULT XexGetModuleSection module=0x71000040 name_ptr=0x8219816c status=0x0 data=0x84099e80 size=0x1080 sha1=2aae5962f6b55ed309919dd3d32c7c2867908e8e
ORIGINAL_DDS_PARSER_REQUEST parser=0x70131090 data=0x84099e80 size=0x1080 info_output=0x0 flags=0x1 lr=0x8250e25c
STOP unsupported-function @0x8253a6a8: sub_8253A6A8: logged_unsupported_instruction
LAST_FUNCTION sub_82536DE0 @0x82536de0 LR=0x8250e25c calls=3112
```

Function-entry totals vary with the existing native workers' timed waits.
The data-flow evidence is the original resource address/size/hash and the
actual parser arguments. The original parser has seven unsupported `bdzf`
instructions at `8253A768..8253A780` in the current generator report. Those
instructions are the next dependency. No texture creation, upload, draw,
Present, title screen or menu transition is claimed here.

## Verification

34/34 native CTest cases and 136 Python tests passed with no skips. The resource
query test failed with its initial throwing stub, then passed. The actual boot
assertion for reaching the parser failed at E960 before the audited exceptions,
then passed after them. Regressions include unchanged payloads, immutable
metadata, exact name matching, absence/error outputs, malformed headers,
address-space boundaries, and complete preflight of both output words.

Logs: `out/xex-resource-red-build.log`, `out/xex-resource-red.log`,
`out/xex-resource-green.log`, `out/texture-wrapper-red.log`,
`out/xex-resource-final-build.log`, `out/xex-resource-ctest.log`,
`out/xex-resource-python.log`.
