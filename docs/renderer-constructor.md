# Original renderer construction and absent debug monitor

Historical checkpoint `2e6acca`; latest progression is [creation affinity](thread-creation-affinity.md).

The Windows original-title/menu goal remains **unachieved**. This checkpoint
executes the original renderer constructor and the game's absent-monitor branch.
It does not add a draw, Present, title screen or menu input implementation.

## Original constructor

The 72 original instructions at `824C1048..824C1164` only capture the device
value; they do not dereference a native graphics layout. The 288 original bytes
have SHA-256 `36b86d3296ef2cb9fc0a518bf6d873c5f0c5a032a1d1de955039b64d6d6fba5b`.
The device travels through r30 into `83E53800`. Other pointer operations use
the original CPU singleton/manager. Entry requires singleton flag bit 0 at
`83E52F40` and manager pointer `83E52ECC == 83E53BE8`. Its field at decimal
offset 884 was allocated by the original CPU allocator and is copied, not used
as a native device layout. The save-helper exception also requires exact
`82A56054`, device `71600000`, and return address `824C1050`.

Original vector instructions write three identity matrices beginning at
`83E53740`; the original caller's subsequent exit registration provides a
read-only observation point. All 48 big-endian float words are checked there.
No matrices, vtables or game state are supplied by the observation code.

## Exported monitor pointer

The [pinned Xenia kernel module](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc)
maps `KeDebugMonitorData` to a valid four-byte cell whose value is null when
no Xbox debug monitor is installed. The original game loads import `820007D4`,
dereferences the resulting pointer and takes its own null branch before
`824D4440`. This runtime installs no Xbox monitor protocol or callback.

The exact name/address/ordinal-89 binding points to checked, read-only cell
`72400000` containing zero. Unprovided neighboring fields and null dereferences
remain rejected. The new cell consumes one actual backing page, so the original
memory-budget calculation now requests `07202000` bytes at `F8AFE000`, one page
less than the preceding checkpoint. The budget is not overridden.

## Reproduction and evidence

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-doubleword
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

`out/renderer-monitor-final-boot.log` captures this actual run (exit 3):

```text
BIND KeDebugMonitorData cell=0x72400000 monitor=0x0 profile=absent-xbox-monitor
ORIGINAL_RENDERER_REQUEST device=0x71600000 singleton_flags=0x1 manager=0x83e53be8 manager_field_884=0xffcffd40
ORIGINAL_RENDERER_STATE object=0x83e53710 device=0x71600000 context=0x83e53720 manager_field=0xffcffd40 identity_matrices=3
REQUEST ExCreateThread output=0x82b50140 stack_size=0x0 id_output=0x70131564 startup=0x824d2a10 worker=0x824d4388 argument=0x82b4ffc8 flags=0x8000001
STOP thread-flags @0x8000001: only original suspended creation is supported
LAST_FUNCTION __restgprlr_25 @0x82a560ac LR=0x824d45e4 calls=3096
```

Eight actual native workers exist, four resumed. The new thread creation is
rejected before publication. Its upper-byte creation flags are the next ABI
dependency to investigate; simply ignoring them is not an implementation.

Validation: 34/34 CTest cases and 136 Python tests passed without skips, including
the real-ROM boot regression; pinned dependencies verified. Logs:
`out/renderer-monitor-final-build.log`, `out/renderer-monitor-ctest.log`,
`out/renderer-monitor-python.log`, `out/renderer-monitor-dependencies.log`.
The monitor test initially failed with the missing implementation, then passed;
the original-renderer boot assertion also failed before the narrow exception.
The first full regression exposed the expected one-page heap-size change;
the assertion was corrected after checking the actual budget/request/result.
