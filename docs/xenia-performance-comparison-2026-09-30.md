# Xenia performance reference investigation — 2026-09-30

The user requested source comparison while unavailable to test the handhelds.
This investigation identifies testable differences; it is not a benchmark of
Xenia and does not establish faster performance on the Ally X or Android.

## Reproducible references

Public source snapshots, downloaded without emulator binaries or game content:

| Repository / branch | Revision |
| --- | --- |
| xenia-project/xenia / master | `95a5c3ee250f80c3b9d139658649d9ffb6db3eec` |
| xenia-canary/xenia-canary / canary_experimental | `c3cd8617b18ef018ea8d638c865e0cced7399846` |

The relevant source files and revision markers are saved locally under
`out/handheld-042/xenia-reference`. No Xenia implementation was copied into the
runtime by this investigation. Existing SFR test inputs enable Vulkan, 720p,
100% render scale, audio, the 60 FPS cap, and vertex caching.

## 1. Vertex transfer and conversion: a measured CPU hotspot

Canary's [SharedMemory::RequestRange](https://github.com/xenia-canary/xenia-canary/blob/c3cd8617b18ef018ea8d638c865e0cced7399846/src/xenia/gpu/shared_memory.cc#L376)
finds invalid guest pages and uploads only required ranges. Its
[D3D12 upload implementation](https://github.com/xenia-canary/xenia-canary/blob/c3cd8617b18ef018ea8d638c865e0cced7399846/src/xenia/gpu/d3d12/d3d12_shared_memory.cc#L314)
copies the bytes to a GPU buffer. The
[DXBC vertex fetch translator](https://github.com/xenia-canary/xenia-canary/blob/c3cd8617b18ef018ea8d638c865e0cced7399846/src/xenia/gpu/dxbc_shader_translator_fetch.cc#L255)
generates endian conversion and format unpacking in the shader after raw loads.
This describes the inspected D3D12 path; it is not a measurement of every backend.

SFR uses native vertex attributes with converted host-format data. Its existing
cache already retains unchanged physical vertex ranges, with write tracking and
safe retirement of old buffers. `SFR_VERTEX_CACHE=1` was enabled in the profile;
late race frames already reuse hundreds of draws. Uncached data still passes
through CPU word swaps, and DEC3N attributes require expansion to SNORM16.

Our main-thread profile attributed 1,007 / 23,006 instruction samples (4.38%) to
`swap_words_into`, despite that cache. This is a sampling fraction, not a predicted
FPS gain. Local commit `f6a5793` removes repeated span-pointer loads from that
conversion loop without changing its instruction-set requirement or output.
Boundary tests and a standalone conversion benchmark are recorded in
[the handheld report](handheld-performance-2026-09-30.md).

Next larger experiment, if transfer costs remain important: quantify uncached
bytes, DEC3N expansion bytes and repeated ranges before considering raw GPU fetch.
A GPU conversion/fetch design changes shader interfaces, bounds and endian
semantics, and backend behavior. It may trade CPU cost for GPU work; it requires
validation on the integrated/mobile GPUs rather than only the RTX 4090. Merely
enabling the existing cache cannot remove this remaining cost.

## 2. Priority: distinguish upstream, Canary and title-specific experiments

The pinned [upstream SetPriority](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xthread.cc#L719)
uses the same thresholds as SFR: >34, >17, <-34 and <-17.
The pinned [Canary mapping](https://github.com/xenia-canary/xenia-canary/blob/c3cd8617b18ef018ea8d638c865e0cced7399846/src/xenia/kernel/xthread.cc#L696)
instead uses >=24 highest, >=17 above-normal, >=10 normal, >=5 below-normal,
otherwise lowest, and SetPriority clamps negative input to zero. Its
[KeSetBasePriorityThread entry](https://github.com/xenia-canary/xenia-canary/blob/c3cd8617b18ef018ea8d638c865e0cced7399846/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc#L390)
forwards the argument unchanged.

Neither reference maps input 16 to above-normal. Claude's >=16 experiment must
therefore be evaluated as its own hypothesis, not described as the same mapping
as this Canary revision. However, Canary maps the title's 0..2 increments to
lowest, so 16 at normal still ranks above those workers. The difference is the
absolute mapping, not whether Canary distinguishes these groups.
Canary's comments/handling also differ from an NT signed
offset interpretation. Check title/kernel expectations before transplanting it.
Codex left scheduling edits in Claude's checkout untouched and recorded this
finding in the shared handoff file.

## 3. Short waits: different rounding, not proof of the handheld cause

Both inspected Windows implementations convert `Sleep(microseconds)` to whole
milliseconds by truncation; values below 100 microseconds explicitly yield.
Consequently 100–999 microseconds also reach `Sleep(0)`. Canary additionally uses
NtDelayExecution for its nanosecond helper. See
[upstream threading](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/base/threading_win.cc#L82)
and [Canary threading](https://github.com/xenia-canary/xenia-canary/blob/c3cd8617b18ef018ea8d638c865e0cced7399846/src/xenia/base/threading_win.cc#L127).

SFR's timed guest waits preserve their requested deadline and use stop-aware
slices. Its revised self-suspension is notification driven. The measured late
worker-7 windows did not show a short-delay polling hotspot. Turning all short
delays into yields could increase CPU/power use or change timing; this source
difference does not justify such a global change. The Ally's ~31.28 ms intervals
remain consistent with several possible bottlenecks, not proof of EcoQoS.

## 4. Intro audio/video: trace two clocks and actual frame progress

Canary's [audio worker](https://github.com/xenia-canary/xenia-canary/blob/c3cd8617b18ef018ea8d638c865e0cced7399846/src/xenia/apu/audio_system.cc#L93)
uses deadlines near 5.333 ms and only executes a client callback when an output
slot is available. Its XAudio2 OnBufferEnd callback releases that slot.
SFR also requests 256 samples at 48 kHz on a wall-clock schedule, but catches up
missed callbacks in bounded batches. Its output implementation checks the host
queue later and discards output if about a quarter-second is already queued.

This is a useful investigation point, not a demonstrated defect: normal music
does not prove the movie decoder or renderer is keeping up. The current movie
hook waits for readiness, and its successful return count is not an independently
verified count of newly decoded images. Neither callback count nor present count
alone measures A/V alignment.

The next useful Intro capture should correlate callback lateness/catch-up,
accepted and played audio samples, decoder output progress and presented movie
timestamps. Preserve the game clock and audio rate while identifying which stage
falls behind. Do not remove the compatibility wait or skip frames merely to
raise the presented FPS count.

## Validation boundary

The localized conversion change has byte-level coverage and a Windows build.
Its 11,500-present scripted capture completed without overlap; three relevant
CTests passed again. One before/after pair averaged 17.9204 / 17.6087 ms in the
late window, but differing race workloads prevent attributing that improvement
to the patch. Details are in the handheld report. Source inspection cannot
replace equivalent-scene Xenia
measurements or actual Ally/Android acceptance. Published releases and the
previously delivered private test package are unchanged.
