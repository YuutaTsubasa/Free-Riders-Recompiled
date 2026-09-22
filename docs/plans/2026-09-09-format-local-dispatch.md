# Original worker formatting dispatch

Actual boot `out/user-country-translation-boot.log` stops in worker3 at
`0x82A68754`. The last-entered getter `0x82A5D3E0` is stale call-entry metadata;
the active original caller is `0x82A68220`, plain bctr at `0x82A68658`, frame1328.
Full evidence: `out/user-country-worker-next-audit.md/json`.

Retain its original56-entry BE16 table at `0x821728A8`, 15 interior destinations,
722 instructions, range checks, state and fall-through. Pin complete original LF
body SHA-256 `f25c067c64663a6d5e2a8f596e6f2bb053d7b6d3c73a160766a441d49805cdd7`.
Only replace the plain bctr call/return with local switch/goto and explicit unknown
target stop; insert12 missing labels and preserve3 existing labels. Do not add
function mappings or hardcode the observed destination.

Preserve three linked bctrl calls and the existing lhzu restoration. Therefore
this separate policy validates and rewrites the original body before ordinary
instruction rewrites; country policy validation stays unchanged. Synthetic tests
cover structural rejection, whole-body/emission hashes, true bctrl/LR preservation,
lhzu, input/log/mapping identity and checkpoint placement. Root runs RED/GREEN,
fresh generation, source audit, native build and genuine entry regression.

This remains a dependency of the original title-to-main-menu goal, not completion.
