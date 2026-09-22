# Executable privilege query plan

> **For agentic workers:** Use subagent-driven-development for XexModule's query/tests, followed by spec and quality review. Root owns import dispatch/integration.

**Goal:** Answer the observed XexCheckExecutablePrivilege query from verified XEX system flags rather than a constant result.

**Architecture:** Add `uint32_t XexModule::check_privilege(uint32_t privilege) const`. Optional-header key0x00030000 is a kind0 immediate whose value is already cached by the validated loader. For indexes0..31 return Boolean `(flags & (uint32_t{1} << privilege)) != 0`, with absent key treated as flags0. Index>=32 throws RuntimeStop before shifting (diagnostic unsupported-input policy, not claimed Xbox behavior). Guest writes to mapped header bytes must not alter cached flags. Dispatch exactname/address __imp__XexCheckExecutablePrivilege/82ACB5AC, return in r3.u64 and log input/result.

- [x] Add tests in tests/xex_module_test.cpp before implementation: missing field, all32indexes with flags0x220, bits0/31 set, unsignedshift31, indexes32/UINT_MAX reject, unrelatedkind1key0x30001 notinterpretedasflags, and guest-header mutation doesnotchangequery. Use temporaryreturn0 implementation to show RED for a setbit, then minimal implementation in src/xex_module.h/.cpp.
- [x] Root add actual-entry assertion `IMPORT XexCheckExecutablePrivilege privilege=10 result=0` and negative oldSTOP assertion, captureRED, add exact dispatch, build with scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-vm, observe/record actual nextstop then add regression.
- [x] FullPython suite with4nativeenvironmentvariables, five CTest targets, bootstrapverify, specandquality review. Updateprogress/README/toolchain/XEXcontract and commit source/config/docs only before continuing.

Evidence: pinnedXenia95a5c3ee250f80c3b9d139658649d9ffb6db3eec src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc22-38 checks a bit position in XEX_HEADER_SYSTEM_FLAGS. PinnedXenonUtils xex.h defines key0x00030000. Actual verifiedheader contains flags0x220, so privilege10 isfalse while5/9aretrue. Actualcaller sub824D2108 setsr3=10 andLR824D212C. PinnedMarathon/Unleashed return0, which is insufficient for this service contract.
