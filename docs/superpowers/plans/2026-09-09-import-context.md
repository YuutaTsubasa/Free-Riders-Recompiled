# Preserve unknown-import evidence

EtxProducerRegister is now the first unsupported import. Public ordinal lists and the two reference ports do not establish its output layout or return contract. Preserve the current stop while making the next reverse-engineering step reproducible.

- [x] Add a boot assertion for an unknown-import context record (name, import address, LR, SP, raw r3 through r10) and observe failure on the vector checkpoint.
- [x] Emit that record immediately before the existing unsupported-import exception. Read no guest pointers and change no guest registers/memory. Keep the exact existing stop category/address/name.
- [x] Read local call sites and initial image data; record observed values separately from inferred parameter roles and unresolved ABI details.
- [x] Review the narrow change, build/run the complete regression suite, and commit source/docs locally. No speculative success/error service implementation.
