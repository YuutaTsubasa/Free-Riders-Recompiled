# Original sampler filtering dependency

Previous goal turn made verified progress (commit656ab54): the original blend
bundle now runs. The current real boot stops at824E8248, LR82211750.
The title/main-menu objective remains unchanged and unachieved.

1. Establish sampler default provenance from the original20-row initializer
   and relevant device setup, including requested anisotropy/volume bytes.
2. Test and implement the complete original min/mag filtering effects for the
   original26-slot domain. Preserve unrelated fetch fields, anisotropic/volume
   effects, actual lookup reads and exact per-slot dirty bits; preflight writes.
3. Expose fresh sampler-filter snapshots from authoritative guest memory so
   original inline mip writes are consumed. Keep original822116A8 running and
   observe its completed inline mutation without altering it.
4. Run the real boot, independent review and relevant/full checks. Save source,
   executable and same-run logs, and continue at the next genuine dependency.

No sampler descriptor binding or successful sampling is claimed by state
retention. Future draw integration must combine real texture and shader sampler
overrides with these current fetch fields.

Completed2026-09-10: full26-slot initialization established, both complete raw
setters implemented, fresh getter and original inline return verified. 65CTest
and192Python tests pass with no skips. The first genuine continuation exceeded
the prior100000-call diagnostic ceiling;250000 plus the existing10-second
watchdog reaches next original824EC0A8/LR8249BC68. No title/menu success claimed.
