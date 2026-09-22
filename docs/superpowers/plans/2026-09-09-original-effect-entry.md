# Original effect entry implementation plan

> **For agentic workers:** Keep the independently audited original function intact; perform a separate quality review of the bounded guard change.

**Goal:** Execute the original FXOBJ effect loader with the actual shader bytes, retaining downstream native-object checks.

**Architecture:** One exact exception in the existing conservative function-entry guard admits original825E7F98 only whenr3equals theknownnative device71600000. It doesnotdecodeorconstructanindependenteffect. A trace event identifiesthisoriginalentry; otherresources/entriesstillstop.

**Evidence:** commit2274f32 realentryread+close ends at825E7F98 LR824B7A44 calls1297. Root andindependentauditor readentireppc_recomp.90.cpp5252..5926. Incomingr3→r20 at5270, onlynullcompares5651/5819, storetonewlyallocatedeffect+700 at5817, movetor3before824F44F8 at5823. No device-basedmemoryaccessorotherdevice-derivedargument. Parsercallsreceiveeffect/blob/allocatedmemory. Original824F44F8 inppc_recomp.78.cpp8166..8178 loads/incrementsdevice+60 andremainsguarded. Other parserdependenciesmaystopfirst; thisisnotproofeffectcreationcompletes.

- [x] AddoriginalboottraceassertionfirstandrecordRED, thenexactdevice-onlyguardexceptionandtrace. No generatedsource/hookforwrapper; originalbodyruns. Unknownr3othernativehandlesstillblocked. Existing824F44F8guardretained.
- [x] Actualentryrecordnextstop, replaceonlyterminalbootassertionsfromobservedrun. Inspectthenextdependencywithoutguessingcompletion. Independentqualityreviewguard+regression; fullappropriatechecks/docs/localcommit. No title/menucompletionclaim.

The first experiment stoppedat purestackhelper82A56044 after1298entries. Root andindependentauditor inspected174.cpp2364..2397: onlyr19..r31/r12 stores tor1-basedstack,nor3use. Addedexactobject-freeexception. SubsequentoriginaleffectparserreachesCreateVertexShader824ED770; checkedreadonlytrace catchesunreadableinspection sooptionalearlycallbackbehaviorisnotchanged. Actualfirstcontainer40019B18 flags102A1101 virt178 physDC, size596 SHA10aa2cf2beb066de65d5cb9bb9b26bb1110487192, matchingassetoffset1124 andexistingintakemanifestSHA2563845a7a1541bd1b6c7642b934b1be2f664fc0619e0ddfc4ed09a748106b5cb15. NextSTOPunsupported824D1858 fromLR824ED808 inoriginalVSresourceinitialization,calls1361. LAST_FUNCTION82A56710 isthepreviousenteredbody, notthestoppingstub. The nextnativegraphicsdependencyisCreateVertexShader, not proofGPUshadercreated.

Independent wrapper/helper audit and separate qualityreview APPROVED. Exactguardnegativecases andunavailabletracepath are coveredbyinspection, notdirectintegrationtests; actualpositivepath/fullregressionverified.
