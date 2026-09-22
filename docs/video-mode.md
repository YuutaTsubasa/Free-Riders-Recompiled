# Native display mode query

The observed import is `__imp__XGetVideoMode` at `0x82ACB23C`. Original function `sub_8249AEB0` requests a stack buffer at `0x70131530`, returns to `0x8249AEC8`, and reads its high-definition and widescreen fields to configure the game's own presentation parameters. The function's subsequent original memcpy remains part of the guest path.

## Sources and boundary

Both pinned reference projects bind XGetVideoMode to VdQueryVideoMode: MarathonRecomp `bd9c0bbd8a99bcc2c0fabdf9521462e75e0ae7d8` and UnleashedRecomp `cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c`, their respective `kernel/imports.cpp`. The 48-byte structure also appears in pinned [Xenia xbox.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/xbox.h#L257). The [reference video query](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc#L120) uses fixed display values. Here resolution and refresh come from the host instead.

The Windows provider uses [EnumDisplaySettingsW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsw) with NULL device and ENUM_CURRENT_SETTINGS. It reads the current display's pixel dimensions, refresh frequency and interlace flag. No display mode is changed. No window is opened by this query. The current implementation supports progressive displays only; query failure, missing fields, unknown refresh0/1, invalid values or interlace cause explicit stops. Non-Windows native display querying is not implemented.

## Guest profile

The output is twelve big-endian words:

| Offset | Value |
| --- | --- |
| 0,4 | Current host width,height |
| 8 | Interlaced (currently only progressive0 accepted) |
| 12 | Widescreen: host pixel aspect strictly wider than4:3 |
| 16 | High definition: host height at least720 pixels |
| 20 | Host refresh frequency encoded as IEEE754 float |
| 24 | Video standard1 |
| 28,32 | Compatibility words0x4A and1 |
| 36,40,44 | Reserved zero |

Standard1 and the two compatibility words retain the reference guest protocol profile; they do not report detected TV circuitry or establish the semantics of undocumented fields. Aspect/HD classification is an explicit native policy. Dimensions must be1..16384 and refresh finite, greater than1 and at most1000Hz. This is current-display information, not a claim that a guest render target or swap chain has been created. A future presentation backend must account for target-monitor selection and mode changes consistently.

The exact name/address handler executes after the existing reservation guard. The void import preserves every PPC register, including r3. A full48-byte checked-write preflight occurs before output stores, so mapping, read-only, import guard and reservation failures cannot partially update the buffer. Null output stops. Unknown imports still stop.

Tests cover the complete byte layout, float representation, aspect/HD classifications, invalid modes and output access boundaries. Real initialization and its next stop must be checked separately; a successful display query alone does not satisfy the title/menu goal.

## Verified Windows execution

A standalone native provider probe and the complete guest boot both reported1920x1080 at60Hz progressive on this host. The true entry queried at70131530 and70131600, then reached MmAllocatePhysicalMemoryEx at82ACBA0C, requesting0x7203000 bytes. Exit3, last function sub_824DF2B8, LR824DF314,1063 function entries. The original presentation-parameter initialization continued; no rendering or input acceptance is claimed.

Evidence: out/video-mode-red-run.log (all four behavior groups fail against the placeholder), out/video-mode-green.log, out/video-mode-native-probe.log, out/video-mode-boot-red.log, out/video-mode-build.log, out/video-mode-boot.log, out/video-mode-tests-final.log (103 tests, no skips), out/video-mode-ctest-final.log (15/15). Pinned dependencies verified. The native-boot regression uses display-value patterns because host settings may change, and pins the observed next stop.
