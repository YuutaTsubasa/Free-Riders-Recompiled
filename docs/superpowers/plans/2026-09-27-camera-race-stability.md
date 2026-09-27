# Camera race stability repair

Goal: reduce false frontal braking and late crouch recovery without replacing the original riding/turn-to-face-sensor gesture; diagnose difficult item throwing.

Evidence: latest 23:46 numeric trace has 1006 pose samples, successful calibration and 15 manager jump pulses. Original Side accepts knee angle <50 degrees and clears jump/crouch via 822C5BB0 mask ~0x1203. 55 samples have front-looking knees but a non-frontal shoulder or pelvis axis. Brake itself only sets 0x100 and requires Side through the original group filter. OverThrow requires wrist above shoulder and forearm up>.8; latest 6Hz trace has zero qualifying samples, while many forward poses already exist. It also clears armed state on <1cm per-update wrist displacement, including repeated camera samples. These observations do not identify all of the user's intended gestures.

- [x] Add a camera-only frontal qualification to the existing Side/Brake path: shoulder and pelvis horizontal direction agree within 30 degrees for 0.2 seconds of fresh observations; 40-degree release hysteresis. Original detector must still recognize the gesture. Suppress throughout crouch/recovery.
- [x] Regression for deep crouch returning only partway to standing first failed. Recognize a clear rise from the tracked lowest point after two qualifying fresh observations spanning 0.04 seconds; do not arm another crouch until standing again. Preserve current crouch entry threshold and data-loss resets.
- [x] Pure and real-hook tests cover neutral/turn/short jitter/disagreeing torso, original detector delegation, P2, deep rise and no duplicate jump.
- [x] Compare original and changed adapters with the same numeric replay in the actual game.
- [x] Review, remove temporary replay injection, run targeted suites, rebuild/stage.
- [ ] Confirm the user's item throwing motion; avoid remapping unrelated item gestures without evidence.

No game/camera images are recorded. Existing user settings and saves are preserved. Changes remain local; no PR/release requested.

Numeric replay at the same 9200-present limit: original Side recognition starts
24 versus gated 7; Brake starts 7 versus gated 1. Both replays reached 6 jump
detector events and exited normally. These are recognition transitions, not
independently labeled intentional/false brakes; the replay is approximate at 6Hz.
Review found single low/high raw leg estimates could masquerade as a rise.
Both regressions failed before fixes and pass now: the minimum must be supported
by two fresh raw heights, and rising requires both raw and filtered height above
the threshold. This final outlier correction is covered by pure/hook regressions;
the actual-game comparison ran before those two outlier safeguards.
Throw behavior remains original pending gesture clarification. Opt-in trace now
includes each original OverThrow arm state change and the detector result, so a
specific throw attempt can be distinguished from missing prep or canceled prep.
