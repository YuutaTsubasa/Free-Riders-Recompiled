# Original XAM system version and dynamic lookup

The real call at8270CE1C compares the result with0x200A3200, then resolves
xam.xex ordinal36 (NetDll_WSAStartupEx). Preserve that original newer-version
branch. Returning0 solely to skip resolution is not a valid implementation.

Declare the native runtime's compatibility target as2.0.12416.0 (0x20308000),
matching both the version and minimum_version of the verified game's XAM and
kernel import libraries. This is a target ABI, not host Windows metadata or a
claim that all Xbox APIs exist. Unimplemented imports/exports continue to stop.
Use explicit packed integer value, not implementation-defined C++ bitfields.

1. Add an original-boot regression for the version query, observe RED against
   current executable, then bind the exact import and return the profile value.
2. Build and boot from the original entry. Observe the actual dynamic module
   lookup and follow its evidence before implementing namespace/exports.
3. Review the bounded change, run appropriate full verification and record the
   real executable/source/log checkpoint. Title/menu remains the full goal.

Source evidence: out/xam-system-version-next-audit.md; original8270CDF8 in
ppc_recomp.105.cpp; image string82000910=xam.xex; pinned xam_table.inc ordinal24hex
isNetDll_WSAStartupEx. Xenia's own version-returning0 is labeledkStub and is not
used as the native runtime version policy.

The real newer-version branch queried xam.xex. Added a bounded named lookup to
the resident NativeModules namespace: capture name before output, publish the
same guarded BE32 identity, and authorize borrowed procedure queries only after
output succeeds. Do not increase load count; null-name executable queries retain
their previous path. Unknown modules and exports still stop.

RED verified for the version binding, named query helper and real-boot query.
Actual boot now obtains71500000 and stops at ordinal0x24 NetDll_WSAStartupEx,
LR8270CE54. Native module tests, full56 CTest/192 Python tests and independent
spec/quality review pass. Full native title/menu requirement remains unproven.
