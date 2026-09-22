# Preserve original eqv initialization

After the original renderer creates its six embedded shaders and sets primitive
restart, the real entry reaches8222B520. Its only unsupported instruction is
plain eqv r7,r4,r11 at8222B534. The full original function must execute.

Resolve only canonical, logged, empty plain eqv instruction blocks. Emit a
64-bit unsigned bitwise complement of XOR into the destination GPR, evaluating
both sources before the assignment; r0 and all aliases are normal registers.
No CR or XER update belongs to this plain form. Record-form and malformed or
partially emitted code remain rejected. Preserve labels and all other failure
reasons. This direct unsigned C++ expression needs no new runtime service.

Test strict operands, source widths, aliases, logs, labels, overlap handling and
other failures before implementing. Verify all actual logged instruction words,
regenerate into a fresh directory, compare input hashes and rejection changes,
build and follow the real next boundary. Completion remains original title to
main menu with real input and same-run evidence.
