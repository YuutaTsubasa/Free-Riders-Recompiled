# Camera race motion repair

Evidence: camera-race-last.log has 200 body samples with +640/+644 zero, changing shoulder X, and hip Y effectively zero. Guest 82439D80 fills those lean fields from player-indexed depth pixels, unavailable to an RGB camera. Original jump reads hip Y velocity, erased by hip-relative pose mapping. Board acceleration reads torso Y/Z without a camera-neutral baseline.

Implement a bounded camera race adapter rather than inventing a depth image or overwriting joint positions with controller fields. At race entry/reacquisition collect a short stable neutral stance. Use lateral torso angle for strictly positive lean ratios at the live body-reader boundary; use sustained forward tilt relative to neutral for Boost, and hip-to-lowest-ankle compression/recovery for crouch and jump. This is estimated pose-based control, not measured Kinect depth or absolute body height. Preserve the actual skeleton in Debug.

Calibration requires a valid upright torso and legs, restarts on significant movement, and produces neutral output until ready. Missing source, invalid poses, manager replacement and leaving a race reset state without synthesizing a release. Crouch requires a minimum hold; jump is a one-frame recovery event. Boost uses hysteresis and a hold interval, and cannot fire while crouching. Other original camera gestures and both controller paths remain in place; camera crouch/jump suppress the conflicting Side/kick detector only during their recognized motion.

- [x] Failing portable tests: neutral, left/right, no spontaneous Boost, deliberate sustained Boost, crouch/recovery, loss/reacquisition, invalid input.
- [x] Add pure camera motion state and integrate at existing race manager, body reader and selected detector boundaries.
- [x] Update real hook tests for camera-only ownership, unchanged joints, P2, and source changes.
- [x] Replay numeric user trace offline, targeted tests, independent review, rebuild for hardware validation.
- [ ] User validation of steering direction, Boost sensitivity and crouch/jump in a real race.

Validation: 14 targeted CTest suites pass. Portable tests first failed neutral calibration, startup leg-estimate settling, reused-pose Boost, and a first delayed observation borrowing neutral time; all pass after fixes. Hook regression first failed camera zero-depth lean, now passes with P2 and XYZ preservation. Numeric trace is sampled at 6 Hz, so replay is an approximate regression, not frame-perfect replay: signed lean covers both directions, zero Boost starts, three compression/recovery cycles recognized (not independently labeled actual gestures). Review caught reused-frame and low-capture-rate dwell bugs; qualification now advances only between consecutive qualifying fresh observations. Stable upright height adapts upward slowly to handle model settling without learning a crouched baseline.

Initial tuning: 0.6 s stable stance, 4-degree steering dead zone, full steer at 30 degrees; Boost additional 18-degree forward tilt held 0.2 s (release at 10 degrees); crouch 20% leg compression held 0.12 s, recovery at 10%. These are camera adapter thresholds subject to hardware validation, not recovered game constants.


Follow-up integration regression: actual game logs showed guest title step +4 = 1,
not seconds. The previous adapter rejected every update and never calibrated.
Camera now measures elapsed seconds with steady_clock; the hook test explicitly
sets title +4/+40 to 1 while advancing an independent host clock. The regression
failed steering before this fix and passes afterwards. Approximate replay of the
453 recorded numeric skeleton samples into the actual game now reaches ready=1,
produces both signed lean directions and crouch states, and exits at the expected
present limit without crashing. No camera images were captured. Tests cannot
establish the feel of the live camera or label the recorded user's intended actions.

Original braking investigation: 822C9BF0 checks stable, separated knees at similar
height/depth and facing the sensor, while 822CA6B0 provides the Side group flag.
Original gameplay is sideways riding / facing the sensor to brake; a frontal
neutral Camera stance conflicts with that design. A wider-stance substitute was
prototyped and tested locally; user preference is pending, so it is not yet an
accepted control scheme.

User chose to retain the original sideways-riding / frontal-braking controls.
The wider-stance prototype was removed from source and tests before staging;
its local scratch copy is under out/experimental-wide-stance-brake only.
The temporary numeric replay include/injection was also removed from nui_hooks.
A separate synthetic skeleton sequence in the actual game reached both lean
signs, Boost, crouch, and one recovery jump. That run included the experimental
brake bridge, so it verifies the clock/adapter integration, not final original
brake interaction; final hook tests cover the retained original detector route.
Live camera motion and sideways calibration still require user validation.
