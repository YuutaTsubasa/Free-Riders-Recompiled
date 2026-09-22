# Missing native party exports and original failure handling

The native XAM namespace explicitly does not offer these six party/community capabilities for the current local title/menu target:

| Ordinal | Xbox export name |
| --- | --- |
| 0xAFF | XamPartyGetUserList |
| 0xB00 | XamPartySendGameInvites |
| 0xB0B | XamPartySetCustomData |
| 0xB10 | XamPartyGetBandwidth |
| 0x305 | XamShowPartyUI |
| 0x30B | XamShowCommunitySessionsUI |

The names are from the pinned [XAM export table](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_table.inc). They do exist on Xbox; this is an explicit native capability policy, not an inference that Xbox lacks the exports. It does not apply to arbitrary unknown services, graphics, gameplay input or game content.

For an acquired native XAM handle and one of these numeric ordinals, `XexGetProcedureAddress` checks the entire non-null output word, writes zero and returns `STATUS_DRIVER_ENTRYPOINT_NOT_FOUND` (`0xC0000263`). No callable pointer or fake party operation is supplied. Unknown handles (including executable handle zero), other ordinals and named lookup remain explicit diagnostic stops, with no output/count changes. Output guards, mapping boundaries, read-only regions and reservations remain enforced.

The error helper implements only `0xC0000263 -> 127`; all other statuses remain unresolved. The original guest code performs the subsequent thread last-error write. `XexUnloadImage` accepts only the acquired native XAM namespace and leaves it resident, with the acquisition count unchanged. Other handles stop. This is not a user-image unloader.

The selected missing-export and kernel-residency behavior follows pinned Xenia's [module services](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc#L121); the status conversion is its [error table entry](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_error.cc#L671). The native module still exposes no LDR fields. The success-only `handle + 0x18` access in the game wrapper must not be bypassed if real exports are implemented later.

The original `sub_82A52C60` statically queries all six capabilities. `sub_824D1DE0` branches on failed status, calls the original error conversion wrapper and returns null; if any capability is missing, `sub_82A52BA8` clears the original object's bindings and calls unload. None of those game branches or data objects is patched by the host.

## Observed execution

The current real entry executes all six missing-export results, six conversions to 127 and the native kernel-module unload call, then continues from 524 to 1,030 function entries. It stops before executing `sub_82211798`, rejected because its `subfze r4,r8` at `0x82211D68` has no emitted implementation. The recompiler log has two events at this address from overlapping definitions; those events must not simply be deleted to enable execution.

```text
UNAVAILABLE XexGetProcedureAddress module=xam.xex ordinal=0xaff output=0x7013fbc0 result=0x0 status=0xc0000263
IMPORT RtlNtStatusToDosError status=0xc0000263 result=127
... (same explicit failures for b00, b0b, b10, 305, 30b)
IMPORT XexUnloadImage handle=0x71500000 resident=1 load_count=1 status=0x0
... (subsequent original initialization)
STOP unsupported-function @0x82211798: sub_82211798: logged_unsupported_instruction
LAST_FUNCTION __restgprlr_23 @0x82a560a4 LR=0x824d2480 calls=1030
```

This yields 18 bounded import handlers, with three Etx unavailable policies separate. The six unavailable exports are capabilities excluded from the native namespace, not implemented party services or additional supported imports. No graphics or input proof exists yet.

Evidence: `out/party-exports-boot-red.log`, `out/party-exports-build-final.log`, `out/party-exports-boot.log`, `out/party-exports-tests-final.log` (94 tests, no skips) and `out/party-exports-ctest-final.log` (12/12). Component tests first failed behaviorally against compiled placeholder methods, then passed with the implementation. An intermediate fixture mistakenly acquired the module after activating a reservation; reordering that setup fixed the test without changing runtime checks. The final native target was rebuilt after that fix.
