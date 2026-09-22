# Original alpha, depth and cull state

Continue from verified2142dce at824E6A08. Adapt all seven audited callbacks in
827F5F40: alpha enable/function/reference, depth requested enable/function/write,
and cull/winding. Preserve original cache masks and all64-bit dirty flags, retain
typed optional native fields without guessed defaults, and preflight every
guest effect before changing either representation. Depth enable is derived
from the retained request and actual known depth attachment, not a stale bool.

Use exact original1/255 float bits3B808081 for alpha reference and require the
same verified image constant. Alpha test state is retained for subsequent pixel
shader specialization; no PSO/shader draw is claimed at this setter boundary.
Hooks preserve the reached void ABI and exact caller LR827F5F88. Test all valid
values and invalid/guard cases, then execute the real entry through all seven
callbacks to the next texture/resource dependency. Audit that next dependency
independently while implementing the current setters.
