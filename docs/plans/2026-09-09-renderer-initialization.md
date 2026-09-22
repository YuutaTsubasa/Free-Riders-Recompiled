# Original renderer initialization and native state dependencies

Commit 6127910 completes 21 original texture creations. Its next stop is
827F61A0/LR82224440 with the native device. The audited entry stores and forwards
the device without dereferencing its layout, but nested calls perform actual
state/resource operations. Preserve those boundaries until adapted.

Permit only device71600000 with the exact entry/caller pair, its stack-only save
helper82A56060/LR827F61A8, and static-caps forwarder82504FA8/LR827F61D0. Capture
the original presentation input for observation; at the first state setter,
compare the original copied caps/presentation and observe actual selected shader
levels. No observer writes guest memory, supplies a return value or claims full
initialization completed. Existing nested native-object checks remain active.

Add real-entry regression expectations, verify RED on the current executable,
then rebuild and observe the next actual boundary. In parallel, audit the
semantics and native representation of824E9218 and its state word before
implementing its native equivalent. Rendering and the title/menu goal remain
unachieved until original draw/presentation/input transitions are verified.

The first real run with forwarding stops earlier at original824D1858, called
from827F63BC to copy64 matrix bytes. Its full body is rejected for two partial
vector stores and two128-byte dcbzl blocks. Implement checked stvlx/stvrx helpers
and128-byte zeroing, match their complete canonical emissions strictly, preserve
all original branches and recompile into a fresh diagnostic directory. The
observed aligned64-byte route uses full vectors, but the complete function must
have valid semantics for every retained instruction. Verify the full copied
bytes and unchanged source at the following original8280C350 call before
continuing toward the blend-state setter. No native memcpy override is added.
