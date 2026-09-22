# Prepare the six original renderer shaders

The verified b37271e native entry stops at 824ED770/LR827F6494 while creating
the embedded vertex container at820D2600. The original twelve declaration
constructors have already returned nonzero. Preserve that original CPU flow.

Extend prepare_shaders with an optional decoded-image input. Keep the existing
basic asset SHA and 68-container validation; validate the complete known image
size/SHA, original six-entry pointer table, and six exact address/size/stage/SHA
records. Apply existing structural validation only to those bounded slices.
Do not scan the rest of the image or substitute existing basic containers.

Generate all74 entries in one fresh private generation, with explicit input
kind and source offset on each record and a source inventory in the manifest.
Keep each original container, HLSL, DXIL, translator/compiler arguments and
hashes. Retain asset_sha256 compatibility and publish only after all entries
validate. Basic-only preparation remains supported; the actual entry test must
require the newly reached three vertex and three pixel creations.

NativeShaders continues matching full original bytes and stage, reserving
opaque handles and owning actual native stages or specialization libraries.
Creation must not invent alpha state or claim a linked PSO/draw. Observe the
twelve original declarations and six original global publications without
modifying guest results. Follow the next actual original runtime boundary.

Tests cover independent source offsets, missing/wrong input, changed table,
changed source metadata/digests, malformed containers, late compilation failure
without publication, provenance, and all74 native resources. Run the real ROM
entry and regression suite. The requested title/menu remains the sole outcome
goal and is not satisfied by this cache or by passing initialization tests.
