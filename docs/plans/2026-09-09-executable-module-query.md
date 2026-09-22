# Original executable module query

180c76c reaches XexGetModuleHandle(0, 701316B0) from original 824D1ED0.
Pinned Xenia xboxkrnl_modules.cc resolves null name to the loaded executable
and returns hmodule_ptr (the LDR_DATA_TABLE_ENTRY), without acquiring a load
reference. Here that is the existing XexModule::module_address 71000040,
not its exported variable cell 71000000 and not the image base.

Implement the observed current-executable query on the already constructed,
validated XexModule. Named-module queries remain explicit unsupported requests
until backed by registry semantics. Check the entire writable, aligned nonnull
output before storing the real existing module identity in big-endian order.
No new module, load count, mapping, or loader fields should be fabricated.

Test current identity/header chain and repeated lookup, boundary/read-only/
guarded/unmapped/null/unaligned output rejection and no writes on invalid names.
Observe RED then GREEN, bind exact import name/address82ACB73C, run real boot
and preserve next dependency. Independent spec/quality review and full native/
Python regression evidence precede local commit. Title/menu is not yet achieved.
