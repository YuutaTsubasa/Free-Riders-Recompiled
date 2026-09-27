# Camera hand action priority

Goal: waving/throwing must not simultaneously become Camera body Boost, kick dash or frontal braking. Preserve original item gestures, steering, crouch/jump and controller/P2 ownership.

Evidence: 00:04 trace has 619 numeric poses, no sampled Camera boost=1, and brake entries with Side+Kick ready/release aggregate bits. Existing body adapter never considered arms. Original Kick uses byte +72; Dash uses uint32 +72 (big-endian), Brake float +72. Both Kick and Dash can write 0x800000/0x1000000; those are not the Brake detector's own bits.

Implementation: CameraArmActivity uses fresh shoulder-relative wrist movement (>0.8m/s and >1cm) or wrist >5cm above shoulder, with valid arm segment lengths and 0.3s tail. Body translation alone is not hand motion. Guard clears body Boost/front dwell and original kick/Brake state before returning neutral. After follow-through, Boost needs neutral pitch and Brake needs shoulder+pelvis agreeing on riding stance, each sustained across fresh observations for 0.12s before their original activation dwell can start anew. Calibration waits for the hand to settle; ready steering is preserved. No item detector is overridden.

- [x] Failing waving+lean/frontal test, then pure guard implementation.
- [x] Real-hook regression checks both Boost routes, Side/Brake, Kick byte and Dash word clearing, original OverThrow delegation, P2 brake.
- [x] Review caught single pelvis/shoulder outlier rearming delayed Brake; regression first failed, now both axes need sustained riding stance.
- [x] Startup raised-hand/lower-to-calibrate, follow-through, body translation, return-to-neutral/riding, camera loss/reacquisition tests.
- [x] Actual game numeric replay and targeted suite.
- [x] Remove temporary replay include/injection, rebuild and stage.

The speed threshold is camera-adapter tuning, not a recovered game constant. The recorded trace is sampled at 6Hz and is not labeled for intentional gestures. Live responsiveness still needs user validation. No camera images, settings or saves are modified; no commit/PR/release requested.

Validation: 14 targeted suites passed. Actual replay exited at the expected
9200-present limit; 82 sampled manager updates had hand protection active, none
with Boost. Original under-swing/under-throw events still occurred. Brake had one
recognized transition in this replay; remaining Kick transitions were not labeled
as intentional or erroneous, so this is not a claim that all live false positives
are eliminated. OverThrow had no recognized transitions; this change does not
claim to repair the previously reported difficult missile throw itself. Production
binary contains no numeric replay injection. Staged SHA256:
80FA0B36E3C462DC72F3BBD26B5EC96DB7849483D3C8E4EF9C883FAC51017C39
