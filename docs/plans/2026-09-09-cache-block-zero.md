# Checked plain dcbz in the original DDS copy

3d8579d reaches original copy routine825F8230, rejected solely for the raw
memset generated from dcbz at825F83CC (word7C005FEC). Pinned primary
out/sthu-xenia-ppc-emit-memory.cc1108 confirms plain dcbz zeros32bytes aligned
down32; its separate dcbz128 operation uses128bytes. Do not infer128fromstride.

Add GuestMemory::zero_cache_block(uint32_t EA): align32, preflight all32bytes
through existing write/import/read-only/reservation checks, zero, then existing
store-completion/cache fence. Keep all bytes untouched on failed preflight.

Rewrite only exact canonical plain dcbz operands and complete expected emitted
memset blocks. RA0 contributes literalzero; RA/RB additions wrap in32bits before
alignment. Reject variants, mismatched memory emissions and extra statements;
preserve comments, labels/checkpoints, raw input and unrelated unsupported logs.
Record retained/invalid instruction counts and independently audit originalwords.

Tests first, runtime and generator RED/GREEN, then fresh generation comparison,
native build, original boot and copy-output observation. Actual mode20001 copies
4096bytes with adjacentbytepairs swapped, not raw byte-identical DDS. Capture
actual callarguments and compare actual destination/source after return before
claiming any copy success. Continue only through original downstream functions;
no synthetic upload, parserreturn, title or menu. Independent spec/qualityreview,
full native/Pythonregressions and localcommit preserve the active whole goal.

Actualcopyreturnedwith4096correctpair-swappedbytes andunchangedsource. Next
deviceboundary8250D6E0 was independently audited with8250C7E8 and82504FA8:
allonlycompare/forward/ignoredevice, noreadsofitslayout. Admitonlyexactdevice/LR
pairs D6E0/E55C,C7E8/D768,4FA8/D858 andretainallotherguards. Observeoriginal
parserSP+160pixelpointer/ownershipafterreturn; executetheoriginalparametersand
allocationpaththroughnextactualstop. No new native texture return/uploadstub.
