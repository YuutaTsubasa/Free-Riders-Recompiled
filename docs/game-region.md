# Explicit native game region

The original entry now passes `XGetGameRegion` using an explicit native runtime
profile. It then returns from the original language-selection constructor and
continues through subsequent initialization to a named synchronization-object
request. There is still no game Draw/Present, title or input-to-menu proof.

## Configuration and original behavior

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-eqv
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

`--game-region=ntsc-us` explicitly configures `0x00FF`. This is a chosen native
application profile, not a detected Xbox hardware fuse or a Windows geography
measurement. Windows UI language remains independently queried; on this machine
it is still `0404`, Xbox language 8. The exact source XEX's security region mask
is `FFFFFFFF`, so it permits this profile. That allowed-region mask is never
returned as the runtime region.

Omitting the region option keeps it unconfigured and stops when the game asks
for it. Unknown, malformed, duplicate or misplaced options fail before guest
entry. The option may also follow the image without an asset argument, but actual
asset access still requires an explicit asset mount. Only this named profile is
implemented; no universal or language-derived region is guessed.

The exact thunk `82ACB24C / __imp__XGetGameRegion` ignores incoming argument
registers and returns the configured uint32 through `r3.u64`. It preserves other
registers and guest memory. Original code still performs the exact `01FC`
comparison at `8222AF00`; the `00FF` profile takes its unequal branch.

The read-only observer at the next original constructor `82213218`, return
address `822118F0`, verifies the completed locale object's fields. The actual
object is `70131D10`: index 0, asset byte 69 (`E`), Japanese flag 0, fallback
byte 69. This is the game's selection after receiving language 8, not a
replacement title screen or a rewritten locale routine.

## Evidence and remaining boundary

`out/game-region-boot.log` records the explicit profile, region return and
original constructor result. It stops at `sync-attributes @70131650`, with
`LR=824D01FC`, after `RtlInitAnsiString` describes the next object's name.
Named synchronization-object creation is the next implementation dependency.

`out/game-region-red.log` and `out/game-region-entry-red.log` record failures
before implementation. `out/game-region-green.log`, `out/game-region-ctest.log`
and `out/game-region-python-final.log` record validation. The configuration and CLI
passed all 45 native tests and 180 Python tests, with no skipped Python tests.
The implementation was independently reviewed, including missing assets, omitted region, unknown
values and exact option handling. `out/game-region-checkpoint.json` binds the
commit, executable hash, command, profile and log.

An additional full run exposed an existing publication test's assumption that
one injected directory-lock error always means exactly two rename attempts.
Windows supplied another transient lock, and publication recovered on attempt
three. That failure remains recorded in `out/game-region-python.log`. The test
now accounts for observed host lock errors, retains the five-attempt bound and
checks each retry; production publication behavior is unchanged. The three
publication tests and final full Python suite passed after this correction.

## Provenance

The North-American profile name and `00FF` value agree in the same index of the
[pinned J-Runner selector](https://github.com/J-Runner-With-Extras/J-Runner-with-Extras/blob/0cb76e079b35c08652d0559f874680bf16e3aa84/J-Runner/Forms/PatchKV.Designer.cs#L188)
and its [value array](https://github.com/J-Runner-With-Extras/J-Runner-with-Extras/blob/0cb76e079b35c08652d0559f874680bf16e3aa84/J-Runner/Forms/PatchKV.cs#L181).
Other labels in that tool conflict with the game's observed Japan fallback and
are not used to construct a broader country table.

The [pinned Xenia API signature](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_info.cc)
provides the zero-argument return ABI; its stub result is not used. Local audits
`out/game-region-next-audit.md/json` and
`out/game-region-continuation-audit.md/json` record the original bytes and XEX
header hashes. The containing original constructor `8222AB40..8222AF40` has
SHA-256 `25d37f044aa851636cdbb954b3caa9f3c55762d017533722a07baa56149eb78d`.
