# Checked bdzf translation for original DDS dispatch

b97d922 reaches original DDS parser8253A6A8 with originalTEX_00 data84099E80,
length1080. Seven bdzf instructions8253A768..780 block the original function.
All1271 bdzf annotations in pinned source currently use4*cr6+eq.

BookI v2.02 bc semantics and existing Xenon fixed32-bit profile require an
unconditional modulo64 CTR decrement, followed by low32-zero AND CR[BI]==0.
No CR/XER/LR mutation for the supported relative nonlink mnemonic. Use a small
portable helper whose runtime tests distinguish high-bit preservation and low32
testing, including false-condition decrement and a seven-way dispatch chain.

The generator accepts only exact canonical empty bdzf blocks with exclusively
matching unrecognized:bdzf log events. Decode CR field/bit and relative target;
require target aligned, within signed16 displacement, inside this function and
an existing label at the exact instruction address. Reject variants, malformed
operands, nonempty blocks, conflicting logs and missing/misplaced targets.
Do not create invented labels or omit remaining unsupported events. Preserve
original instruction comments and existing label scheduling checkpoints.

Add semantic runtime and strict generator RED/GREEN tests. Generate a fresh
diagnostic directory, compare all input/log fingerprints and retention changes,
audit original instruction words, build and run the original DDS path. Independent
spec/quality review, full native/Python regressions and local commit follow.
Original title/menu remains the active unachieved goal.
