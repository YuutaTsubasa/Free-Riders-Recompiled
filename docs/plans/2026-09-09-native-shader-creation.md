# Native shader creation from original effect flow

Goal remains the original Windows title -> input -> original main menu. This
dependency is complete only when original shader creation requests acquire
retained native shader resources from reproducibly translated original bytes.
It is not evidence of a rendered frame.

## Design

Use the Marathon-pinned XenosRecomp translator at
`fb32631ee398e46f2a113d8f9103201dbaa000b4`. Build only its translation unit with
the existing pinned fmt dependency; omit its unrelated cache/compression CLI.
The tracked preparation command validates the known basic shader asset and
container bounds, translates all 68 containers, compiles zero-specialization
shaders directly to shader model 6.0 and other shaders to DXIL libraries. It
publishes a generated C++ cache only after every translation/compile succeeds.
All original/generated shader bytes remain ignored. The manifest records tools,
source/output hashes and exact arguments. No output from the earlier probe is
trusted as a production input.

Native creation matches the complete container bytes and stage against that
compiled cache. It owns direct Plume shader objects for ready stage blobs and
retains specialization libraries until real render-state-based linking. Missing
or different shaders fail explicitly. Opaque guest handles carry no invented
Xbox driver resource header. Original effect parser and callers still execute;
any helper using handles must be audited before it can cross the graphics guard.

Alternatives rejected: execute Xbox shader allocator/copy merely to postpone its
driver boundary; use a shader or UI unrelated to the requested original shader;
apply an unobserved zero pixel specialization state. Runtime translation is
unnecessary for this first known static asset and complicates reproducibility.

## Tasks / verification

- [x] Audit original CreateVS/PS ABI and immediate resource consumers.
- [x] Promote reproducible pinned translator/cache preparation with malformed
      input, compilation failure and publication tests; convert all real records.
- [x] Add cache/resource ownership and exact public shader hooks with meaningful
      native tests (matching, unknown, wrong-stage, metadata and lifetime).
- [x] Build Windows native diagnostic and execute the same original entry with
      mounted real assets. Record actual shader creation and next stop.
- [x] Independent specification and quality review, full relevant regression
      checks, update evidence/status and local commit. Never mark title/menu done
      on shader creation alone.

Local implementation and review continue under the user's standing authorization.


Completed evidence: all68 original requests acquire native resources;
originaldeviceAddRef subsequently runs. Currentstop825E47D8/LR825E8428,
1419entries. Native25/25 and Python117/no skips pass. See docs/native-shaders.md.
The local checkpoint commit follows this verified implementation.
