# Render thread

`NativeRenderer::draw` used to record its commands (`setPipeline`, descriptors,
`drawIndexedInstanced`) on the guest's main thread, about 1.5 ms of a race
frame. `NativePresentation::record_async` hands that recording to a thread of
its own.

- The body is copied into a fixed-size slot (256 bytes) of a single-producer,
  single-consumer queue of 4096 slots, together with the viewport, scissor and
  `list_generation` the caller saw. It must be trivially copyable and capture
  values, never references to the caller's locals.
- Everything else that touches the open command list (`record`, `clear`,
  `draw_player_model`, `present`, `flush`, beginning a list, the work before a
  submission including `before_submit` callbacks) first waits for the queue to
  empty, so the commands land in the order they were asked for.
- Uploads (texture batches, the constant ring, vertex data) and pipeline
  creation stay on the calling thread. The GPU sees the same commands in the same
  order.
- A failure on the render thread is rethrown by the next `record_async` or
  `drain()` on the calling thread.

## Switch

`SFR_RENDER_THREAD=0` records on the calling thread as before, `=1` forces the
thread. Unset, it is on for D3D12 and off for every other backend: it was only
measured on D3D12. The Vulkan path was exercised only by the ordering test
below on a software driver, never on a device.

## Measurements (v0.4.5, D3D12, six interleaved rounds each)

| PC | Render thread off, fps ratio to on | Draw ms per frame, on / off |
| --- | --- | --- |
| i5-3470 / RX 480 | median 0.88 (5 of 5 clean rounds slower) | 5.3 / 7.4 |
| Ryzen AI MAX+ 395 | median 1.00 (no effect) | 1.6 / 1.8 |

It helps a slow CPU and costs nothing on a fast one. Not re-measured on v0.4.6.

## Test

`async_records_keep_the_order_of_everything_asked_for` in
`tests/native_presentation_test.cpp` forces the thread on, then alternates
`clear`, an asynchronous clear and, every other round, another `clear`, reading
the colour back: the last command asked for must be the one the GPU holds.
