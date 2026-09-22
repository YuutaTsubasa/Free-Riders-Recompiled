# Original user-language configuration query

The11395fe entry reachesExGetXConfigSetting(3,9,buffer,4,required) at82ACB5FC,
LR824D0B84. The original wrapper requires a four-byte language but initializes
a two-byte required-size cell. Preserve the full original wrapper and callers.

Snapshot Windows GetUserDefaultUILanguage and explicitly map the supported
Xbox language IDs1..12. Distinguish Chinese sublanguages; do not derive language
from timezone or guess English for an unknown/custom language. Unsupported host
mapping stops explicitly. The original game keeps its own language normalization
and region fallback; do not replace those branches.

Implement only auditedUSER_LANGUAGE3/9. Other queries stop before effects rather
than copying a reference emulator's placeholders for unrelated settings. Inputs
category/setting/capacity are low16, guest addresses low32. The language is BE32,
optional required-size is BE16. Success/null-size-query publishes required4;
short-buffer and null-buffer/nonzero-capacity return the verified failing status
and required0, without touching language output. Preflight every effect first,
then preserve language-before-required store order even when outputs overlap.

Use native and pure tests for language mapping, exact output widths/order,
capacity/null cases and guard/atomic failure behavior. Prove RED then GREEN;
integrate the exact import address/name and log real native language provenance.
Run the actual ROM entry and next original dependency, all regressions and an
independent review. No title/menu completion claim until actual rendering and
input evidence proves the original end state.
