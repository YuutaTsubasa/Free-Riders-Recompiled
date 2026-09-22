# Counted ANSI string initializer implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development, followed by spec and quality review.

**Goal:** Let the real game construct the filename descriptor for its original basic-shader asset, continuing from the new viewport boundary.

**Architecture:** A small portable GuestMemory helper writes the verified eight-byte Xbox ANSI_STRING after complete destination/source validation. The exact import address/name uses its void ABI and preserves PPC registers. It aliases the original guest source rather than allocating or copying string contents.

**Tech Stack:** C++20, existing checked big-endian GuestMemory, original-entry integration test.

Evidence: `out/native-viewport-boot-next.log` reaches RtlInitAnsiString82ACB57C, LR824D0FB8, r3destination70131318, r4source821A8FE4 after1265entries. Verifiedimagebytes atsource are `game:\shader\Xbox360BasicShader.fxobj`. Original caller inppc_recomp.73.cpp reads BE16length atdestination before preparing NtCreateFile. PinnedXenia95a5c3e xboxkrnl_rtl.cc141 andxbox.h227 define BE16length,+2maximum_length,+4BE32pointer,size8. Fornon-nullsource bothreferencegames use strlen andlength+1. Fornullsource Xenia andMicrosoft specifyallzero; referencegames setmax1 evenfornull, so use the former explicitnullcontract. Overflowsemantics differ (referenceuint16wrap versusMicrosoftsaturation), so this bounded adapter accepts lengths0..65534 andexplicitlystops ifnoNULwithin65535checkedbytes. No sourcepointerwrapping, no guessedtruncation.

- [x] Agent owns src/ansi_string.h/.cpp andtests/ansi_string_test.cpp only. API void initialize_ansi_string(GuestMemory&,uint32destination,uint32source). Validate complete8bytedestination before source access; non-nullsource scan checkedbytes withuint64address arithmetic untilNUL; maxlength65534. Builddescriptoronlyaftervalidation; writeBE16length/max andBE32source. Nullsource =>0/0/0; nonnull empty=>0/1/originalpointer. No hoststrlen onguestmemory, noallocation/copy, no registerwrites. Do not add unproven alignment restrictions.
- [x] Agenttests first: exact8BEbytes, sourceunchanged, nullvsnonnull-empty, high-bitANSIbytes count, NULatlastmappedbyte, boundarylength65534success/65535rejectbeforemutation, unterminated/missing-source/truncated/readonlydestination and4GiBwrap rejection allleaveoutputunchanged. Use existingtests simpleexceptions/mainpattern. NotifyTEST_READY forrootREDbuild, thenimplementation, STABLEforrootbuild. No owncmake/ninja.
- [x] RootCMake/buildscript/testtarget andexactimportdispatcher82ACB57C/__imp__RtlInitAnsiString. Originalentrytest REDrequiresIMPORTlog ofdescriptor/source/length/max; no statusreturn. Realbootthenlocatesnextactualservice; genericfileopenisnotfaked.
- [x] Separatespec/qualityreviews, appropriatefullnative/Python/pins, docs andlocalcheckpoint. User's continuousautonomyapplies; no routine approval pause. Originaltitle/menu remains incomplete.
