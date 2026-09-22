# Original blend request initialization and setters

The actual native boot reaches original `824E6A40` from `8221171C` and stops before alpha-blend enable. Implement only dependencies supported by original code and default-table evidence. Title/menu success still requires original rendering and input in one run.

1. Verify original creation/default setter provenance for requested words at device `+2EF8/+2EFC`; validate relevant original table rows before native initialization. Keep unrelated states explicitly unsupported.
2. Add adversarial tests for the three complete ABI setters: enable `824E6A40`, source `824E6B60`, destination `824E6BF0`. Cover requested/effective separation, all four targets, exact dirty bits, raw DWORD behavior, guards and transactional failure.
3. Implement exact supported state effects, hook only verified entry addresses, and observe the real game sequence. Preserve original caller execution and stop at the next unsupported dependency.
4. Run native/Python checks, obtain independent review, record source/executable/log evidence and commit locally. No push. State retention does not constitute a draw or title screen.

Evidence: local `out/graphics-824e6a40-next-audit.md` and `out/blend-cache-initialization-audit.md`; verified original image SHA256 `d092c5397d769594d7b46cda023cf2c62aed9df4e76db44efee8136daf00ad99`.

Completed 2026-09-10: verified 11 source rows, implemented all three setters, corrected observation reads and per-setter target order after independent review. 64 native and 192 Python tests pass with no skips. The final original boot passes the three requests and stops at sampler min-filter setter `824E8248`, LR `82211750`. Next dependency audit includes mag-filter and the original inline mip write. Overall title/menu objective remains unachieved.
