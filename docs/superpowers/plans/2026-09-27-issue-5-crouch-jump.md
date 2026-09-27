# Issue #5: preserve crouch and jump during pad-controlled races

> For agentic workers: use subagent-driven-development for the bounded hook/test change and requesting-code-review before publishing.

**Goal:** Holding A charges a crouch and releasing A jumps without an unrelated physical-pose detector cancelling the input.

**Architecture:** Override the remaining `Side` detector (`822CA6B0`) through the existing `RACE_DETECTOR` boundary. B supplies the pad-driven side/brake state; the inactive branch preserves the original auxiliary flag. Keep original guest detection outside pad racing, body tracking, kick inputs, and the game's conflict filter unchanged.

**Tech stack:** C++20, guest-memory hook tests, scripted Windows Vulkan runtime.

## Evidence

A controlled Free Race run with B/X neutral produced 20 A hold/release cycles. The Side detector added `0x400000` from copied skeleton geometry whenever A marked both hands tracked. The crouch detector emitted `0x7000`, but the original group filter removed `0x1000`: all 860 charged frames ended with `0x446000` instead of `0x47000`. All 20 release events reached `0x200`; they followed cancelled crouches. This reproduces a defect consistent with the report; the reporter supplied no platform/version.

## Plan

- [x] Trace input, individual detector writes, and final result flags in the actual game.
- [x] Add a game-file-free hook regression test; demonstrate failure before the fix.
- [x] Override Side for pad racing; retain the original active/inactive result contract.
- [x] Verify A, A plus downward stick, B braking, X kick edges, and original fallback.
- [x] Repeat the runtime sequence and compare final crouch/jump flags.
- [x] Build/test on Windows and Linux and complete independent code review.

## Verification

- The hook regression failed before the override with `A must not be classified as Side` and passed afterward. It runs the actual controller hooks with guest-code boundary fixtures, without game files.
- Repeated 20 scripted A hold/release cycles in Free Race, alternating neutral stick and stick-down held through release. Before the fix, all 860 charged frames lost crouch and 10 of 20 jump edges were cancelled. After the fix, all 860 charged frames retained crouch (`0x47000`) and all 20 jump edges survived (`0x200`). These are detector-result measurements, not trick-score assertions.
- Runtime comparison used an instrumented Windows Vulkan diagnostic linked with existing game objects. The controller input and hook sources in those objects' checkout match the main baseline; the replacement hook object comes from this fix.
- Windows build and 91/91 CTest tests passed; Linux build and 86/86 passed. Python: 219 tests, 11 skipped. After adding explicit A+B and A+X priority cases, the hook target was rebuilt and passed again on both platforms.
- Independent review found no actionable issues. The reporter's specific device/version and trick-score behavior remain unverified.

The Windows build script also includes seven previously omitted existing test targets so a clean build can run the complete registered suite.

Temporary instrumentation and generated/private game data remain under ignored `out/` paths.
