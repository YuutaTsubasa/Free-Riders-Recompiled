#include "diagnostic_hooks.h"
#include "ppc_recomp_shared.h"

// This periodic callback only derives Xbox Live user contexts (location,
// mode, activity) and sends them through 824D0638 / XGIUserSetContextEx.
// XMsgStartIORequest already accepts and drops those writes: there is no
// Live backend. Do not assemble unused presence from mutable scene objects.
// In particular, 82246818 publishes 83E52F8C before assigning its +80 mode
// pointer, which this callback dereferences from a different guest core.
// The sole caller, 822343A0, ignores its return value; it has no gameplay
// writes. Revisit this hook if a presence backend is ever implemented.
SFR_CONCURRENT_HOOK(sub_82232600) {
    sfr::enter_function(ctx, "sub_82232600", 0x82232600);
}
