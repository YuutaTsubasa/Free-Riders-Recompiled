# Original asynchronous asset opening

The real worker reaches NtCreateFile from 824DE904 (LR 824DE908), through
824DEB24 / 824D108C, requesting `game:\FNT_SE`, access 80120089, sharing 1,
disposition 1 and options 48. The extracted original asset is 14774 bytes.

1. Verify native and guest tests fail against the unsupported implementation.
2. Retain actual read-only Windows OVERLAPPED / NO_BUFFERING handles, including
   existing path containment, sharing, close ownership and metadata behavior.
   Query actual NT access/mode and filesystem alignment for provenance.
3. Accept only the evidenced guest asynchronous profile alongside the existing
   synchronous profile. Reject synchronous reads on asynchronous handles until
   actual completion ownership is implemented.
4. Build and run the original entry; capture its next real file operation.
   Xbox class 27 is XCTD information, not Windows compression information.
   FNT_SE has the original XCTD header, so do not synthesize an uncompressed
   success that would bypass the original header parsing.
5. Run regressions, review and record exact executable/log evidence. Opening
   an asset alone does not satisfy the original title-to-menu goal.

Implemented and verified: actual asynchronous open, source-backed unavailable
Xbox class27 response, and the observed null-output NtSetEvent import. The
original producer wakes worker10, which now requests a 0x20000-byte read at
LR824DD208. Native completion remains unsupported. Full 51 CTest and 188 Python
checks pass; see ../async-file-startup.md for the same-run evidence and next
completion/lifetime requirements.
