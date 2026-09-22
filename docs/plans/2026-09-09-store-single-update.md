# Original renderer stfsu dependency

The real entry now completes all eight original null texture bindings and stops
at 82811050, whose two missing stfsu instructions are at 82811188/82811268.
The original words are both D41F000C: store f0 at r31+12, then update r31.
Preserve the entire original function and its preceding fmuls instructions.

Use the FPR's raw uint64 bits and PowerPC Book I section 4.6.3's integer format
conversion; do not replace it with a host float cast. Nonzero exponents below
874 are architecturally undefined and must stop before changing memory or RA.
Follow the existing scalar memory profile: full64 effective address arithmetic,
low32 guarded big-endian store, then full64 base update. Retain existing scalar
alignment behavior without claiming precise hardware exception emulation.

Accept only logged canonical empty stfsu blocks, f0..31, r1..31 and signed16
displacements. Canonical labels cannot hide partial emitted code. Preserve
other rejection reasons and inspect every retained/rejected function change.
First obtain failing generator and native tests, then implement, review and
regenerate into a new ignored directory. Run the real entry and all regression
tests; report its next actual boundary without claiming title/menu completion.
