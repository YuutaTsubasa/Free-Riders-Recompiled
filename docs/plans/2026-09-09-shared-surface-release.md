# Continue original shared-surface release

The2a9502e run completes original texture transfer and unlock. It then stops in
824F19E8 reading VdGlobalDevice. Actual destructor4002FEA0/LR824F1FB8 has header
40100004, refcount0, fence0 and parent400007C0. The original shared-surface branch
ignores the loaded device and frees only its CPU header; recursive parent Release
has already executed. Preserve that original path rather than replacing Release.

Implement the verified export ABI as a separate writable4-byte,32-aligned cell
initialized BE0. Resolve IAT82000664 only for the audited destructor context,
saved original caller, and shared-surface/refcount state. Unknown consumers keep
an explicit import stop. No native device pointer or invented ringbuffer state
is published. Unit tests cover writablecell ABI, exactresourcevalidation and
rejections before access; actualboot verifies original heap free and nextcaller.

Audit the original return gate before recording completion. Run focusedRED/GREEN,
the genuine original entry, full regressions and independent review; commit
locally. Title/menu completion still requires original rendering and input.
