# Critical-section spin-count initialization plan

> **For agentic workers:** Use subagent-driven-development for the initializer/tests; root owns import integration, then obtain independent spec and quality review.

**Goal:** Advance CRT initialization through RtlInitializeCriticalSectionAndSpinCount using checked guest data and a source-backed conversion.

**Architecture:** Add `uint32_t initialize_critical_section_and_spin_count(GuestMemory&,uint32_t address,uint32_t spin_count)`. For spin_count<=0xFFFFFF00 compute `(spin_count+255)>>8`, clampto255 and storethebyteat+1 after calling the existing full28bytechecked initializer. Return status0 (Xenia X_STATUS_SUCCESS). Inputs>0xFFFFFF00 stop before writing because the pinned reference's uint32 addition would wrap; this is a documented diagnostic restriction. Unknown memory, guarded tails and invalid ranges retain existing stops. Header bytes2/3,8..15 stay preserved. No wait/contended spin implementation is implied.

- [x] Add tests before implementation in critical_section_test.cpp: 0,1,255,256,257,4000,65280,65281,0xFFFFFF00 convertto0,1,1,1,2,16,255,255,255; exact28byteinitialization andstatus0, preservedfields/neighbors; overflow-rangeinputs0xFFFFFF01/FFFFFFFF rejectunchanged; guarded/unmappedtail rejectunchanged; resulting initialized nonzero-spin lock supports existing Enter/Leave and keeps spinbyte. Emptyfunctionmustfail4000byteassertion first; implementminimalreuse in src/critical_section.h/.cpp and runisolatedGREEN.
- [x] Root addactualentryexpectedspin4000/status0assertion, captureRED, dispatchonlyname__imp__RtlInitializeCriticalSectionAndSpinCount/address82ACC2DC. Readsectionr3/spinr4beforecall, returnuint32statuszeroxr3, logsection/input/encodedbyte/status. Rebuilddiagnostic-vm and runactualimage-loader, assertnextobservedSTOP/LR/count; neverchangeguestcaller.
- [x] RuncompletePythonwithallnativeenvvariables, fiveCTesttargets, bootstrapverify, independentreviews, updatecontracts/progress/README andcommitsource/docsonly.

Evidence: fixedXenia95a5c3ee250f80c3b9d139658649d9ffb6db3eec xboxkrnl_rtl.cc394-418 roundsup256units, clamps255, initializesfields andreturnsX_STATUS_SUCCESS. ReferenceMarathon/Unleashed treatreturnasvoid andlackclamp; followXeniaforstatusandclamp. ActualFreeRiders sub82A62C68 setsr4=4000 beforewrapper82A62BD0; wrapperreplacesr3with1 afterimport, so correctstatus0isnotitswrapperBooleanreturn. LR82A62BF4, guest firstsectionderivedfromlis-32061/addi6904. Boundadditionoverflowsasunsupportedratherthanassumingwrapisintendedconsolebehavior.
