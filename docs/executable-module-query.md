# Current executable module query

Historical checkpoint `6da84b4`; latest progression is [embedded resource lookup](xex-resource-query.md).

At checkpoint `180c76c`, the original game calls XexGetModuleHandle with null
name and a guest output pointer. This refers to its already loaded executable.

The [pinned module export implementation](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc)
returns the current executable's `hmodule_ptr()` without retaining a handle.
The [module definition](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xmodule.h)
identifies this as the loader table entry. In this runtime, that is the existing
`XexModule::module_address` (`71000040`), whose supported header pointer at +58
resolves to the validated original XEX header. The exported variable cell
`71000000` contains that identity but is not itself the module handle.

The query operates on the already constructed XexModule. It checks the complete
aligned, non-null output and stores the existing identity big-endian. It adds
no mappings, references or load count. Named lookup remains an explicit
unsupported request; unknown names are not falsely declared absent. Unsupported
loader fields remain guarded.

The title/menu goal remains unachieved; a successful module query is only a
dependency in the real original startup.

## Actual boot and verification

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-doubleword
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

`out/executable-query-boot.log` records exit 3 and:

```text
RESULT XexGetModuleHandle name=0x0 output=0x701316b0 status=0x0 module=0x71000040 retained=0
IMPORT_CONTEXT name=__imp__XexGetModuleSection address=0x82acb72c lr=0x824d1ea8 sp=0x70131660 r3=0x71000040 r4=0x8219816c r5=0x70131754 r6=0x70131750 r7=0xffffffff824deb00 r8=0xffffffff82b30000 r9=0x8270d980 r10=0x821a9c48
STOP import-function @0x82acb72c: __imp__XexGetModuleSection
LAST_FUNCTION sub_824D1E98 @0x824d1e98 LR=0x824d1ea8 calls=3114
```

The actual name at original image address `8219816C` is `TEX_00`. Its entry
exists in original XEX resource header `000002FF` (file offset `330C`, length
356, 22 entries), with original address `84099E80` and size `1080`. The resource
query is the next missing dependency; it has not been implemented here.

The XEX unit test failed with the missing implementation, then passed with the
real lookup. Full native CTest is 34/34; Python is 136 tests, no skips, including
the true-ROM boot. Existing original workers still execute and wait as before.
Tests verify repeated identity, original header linkage, output byte order,
unchanged mappings, invalid outputs and names without writes, guarded loader
fields, and exact logical/address-space boundaries.

Logs: `out/executable-query-red-build.log`, `out/executable-query-red.log`,
`out/executable-query-build.log`, `out/executable-query-green.log`,
`out/executable-query-ctest.log`, `out/executable-query-python.log`.
