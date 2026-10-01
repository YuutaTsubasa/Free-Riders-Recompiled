# Batched texture uploads implementation plan

**Goal:** Remove per-texture GPU submissions and four-slot reuse waits while
preserving texture contents, copy-before-draw order and bounded staging memory.

**Architecture:** A renderer-owned two-slot NativeUploadBatch records copies
on the existing graphics queue. NativePresentation before-submit callbacks
submit pending copies before graphics execution, including an otherwise empty
flush. A complete after-flush finishes upload fences; asynchronous presents
leave two slots in rotation. A 32 MiB retained-staging budget drains uploads
without submitting the partially recorded graphics list. A single oversized
upload is allowed and drained immediately. Legacy path remains selectable
with SFR_TEXTURE_UPLOAD_BATCH=0; SFR_TEXTURE_UPLOAD_WAIT=1 retains its old meaning.

**Tech stack:** C++20, Plume Vulkan/D3D12, CTest, Android ARM64/ADB.

- [x] Implement and GPU-test NativeUploadBatch in a separate helper. Interface:
  constructor(device,queue,budget), record(staging,bytes,callback(list,buffer)),
  submit(), finish(), stats() with cumulative submissions/wait_ms/peak_bytes/
  budget_drains. No reference implementation copied. Destructor discards
  unsubmitted commands and waits submitted copies before releasing staging.
- [x] Test and add NativePresentation before-submit hooks: run before graphics
  submission and even when flush has no open graphics list. Clear callbacks
  with renderer destruction. Existing readback/resize/shutdown behavior stays.
- [x] Integrate renderer copies and complete-flush retirement. Do not flush the
  vertex ring when the staging budget is hit. Preserve placeholder/legacy path.
- [x] Verify more than four copies, two-slot reuse, memory-pressure drains,
  immutable earlier upload bytes and actual GPU output. Cover Vulkan/D3D12.
- [x] Build Windows and Android. Run Thor old/new matched fixture, record
  submission/wait/memory metrics and normal screenshots. Treat FPS differences
  with scene progression differences as inconclusive.
- [x] Review, restore normal APK without debug.env, document evidence/limits
  and commit locally using noreply; no push/release requested.

Results: docs/performance.md records dual-backend GPU readbacks and all three
Thor runs. Batch mode merges 1,005 copies into 163 submissions, with 7.171 ms
recorded upload waits and 31.851 MiB retained staging peak. Sustained FPS gain
is not established; aggregate P95 remains similar and scenes differ. Normal
APK restored, debug.env removed, source review completed.
