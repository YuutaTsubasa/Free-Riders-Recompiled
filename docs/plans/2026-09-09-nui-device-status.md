# Original NUI device status dependency

Continue the user-authorized Windows title/menu work from eeca000. The original
wrapper allocates a 24-byte output and consumes its BE32 status at offset12.
Pinned Xenia's void ABI defines six words and status0 for no connected device;
unknown fields are zero in that absent-device profile. It is marked a stub in
Xenia, so this is not evidence of a complete sensor protocol.

Use an explicitly absent backend while the native runtime has no sensor input.
Do not advertise a connected sensor or manufacture skeleton data. Preserve the
original caller's branches so the next original dependency is observable. Host
keyboard/controller adaptation remains necessary for the full title/menu goal.

1. Add a 24-byte checked output helper and meaningful RED tests: exact range,
   surrounding bytes, final guest page, full preflight before writes, import
   and computed read-only guards, pending writes, and overflow/unmapped refusal.
2. Implement the absent profile, bind exact import address/name, preserve void
   return behavior, and log output/backend/status. Test GREEN before real boot.
3. Launch the existing generated original program and inspect the original
   downstream behavior. Validate tests, review, and record source/executable/log
   evidence. Keep unknown services as visible stops and leave full goal active.

Evidence: out/nui-device-status-next-audit.md, out/xenia-pinned-xam-nui.cc and
the original sub_824D0D70 wrapper in ppc_recomp.73.cpp. No new generated code is
needed for this import binding.

Implemented and independently reviewed. Both helper and original-boot RED
checks failed for the missing implementation; GREEN and all56 CTest/192 Python
tests pass. Actual boot completes two absent-device queries, then stops on
XamGetSystemVersion82ACB35C at LR8270CE1C. Full title/menu goal remains active.
