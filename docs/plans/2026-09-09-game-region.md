# Explicit native game-region configuration

Continue the original Windows entry through XGetGameRegion at 82ACB24C,
return address 8222AF00. The API returns the native application's configured
console-compatible region, with no guest-memory effects. Do not infer a console
hardware value from Windows language, geography, or the XEX allowed-region mask.

Add an explicit optional launch argument `--game-region=ntsc-us` with value
00FF. This value is independently documented by the pinned J-Runner region
selector; the verified local US/Europe XEX allows all regions. It is a deliberate
native runtime profile, not a measurement of the user's Windows hardware.
No argument leaves the region unconfigured and the query stops explicitly;
unsupported options fail before guest entry. Keep existing two positional
arguments (image and optional assets), with this option appended. Accept the
option directly after the image when assets are omitted; reject duplicates and
an explicitly supplied empty option before guest entry.

Use a small portable parser/value owner; independently test valid configuration,
missing/unknown/malformed options and deterministic read-only return. Add native
entry regression with the explicit option and preserve a no-option stop test.
Keep all original language and region callers unchanged. Prove RED, implement,
verify real execution beyond the query, then investigate the actual next stop.
Record the exact command/profile/source version/log in the checkpoint. Review
the import ABI and configuration independently; complete full regression tests.

This is only an initialization dependency. It does not establish any title,
Draw/Present or input-to-menu completion.
