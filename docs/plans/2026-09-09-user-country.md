# Actual user-country configuration and original translation

Continue the actual ExGetXConfigSetting category3/setting14 request at82ACB5FC,
LR824D1BD4. Query the Windows user geographic name with GetUserDefaultGeoName
and map its uppercase ISO2 value to the pinned Xbox user-country table. Numeric
M49, unknown/custom/unmapped values stop explicitly. Do not infer country from
Windows UI language or the separately configured native game region.

Add an optional validated country snapshot to SystemConfig. The country output
is exactly one byte; required-size output remains BE16. Preserve the existing
language query and the established short/null-buffer statuses, complete output
preflight, overlap store order and no effects for unsupported/unconfigured queries.

Prove native mapping and query ABI tests RED before implementation. Build and run
the actual original entry. Audit predicts the country translator824D1BA8 uses an
internal CTR jump into its own body; if the real run reaches this missing dispatch,
retain the original table and restore its branch semantics, never select a country
to avoid the branch. Record actual geographic provenance and eventual original
return value at824D0B58/LR82439678. Full regression/review and local commit follow.

No title/menu completion until original graphics and confirmation input are proven.
