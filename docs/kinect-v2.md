# Kinect v2: how it works today, what is open, and how to trace it

Date: 2026-10-18 (after v0.10.3). For whoever picks up the Kinect v2 work with
a real sensor. The longer Chinese notes are in [kinect-sensor.md](kinect-sensor.md).

Nobody on the project had a Kinect v2 while this was written. Everything about
v2 here comes from code reading, unit tests, and `game.log` files that one
player (tameradel578-cmyk) attached to issues #97, #132 and #165.

## Words used here

- **Body**: one person the sensor tracks. A v2 tracks up to six.
- **Joint**: one point of a body (hip, hand, ...), in metres, in the sensor's
  own space: +x to one side, +y up, +z away from the sensor.
- **Skeleton frame**: one sample of all bodies. A v2 sends 30 a second.
- **Detector**: one of the game's own ~25 gesture checks (jump, crouch, ...).
  See [gesture-detectors.md](gesture-detectors.md).
- **Body record**: a 4788-byte block the game fills from the skeleton every
  frame. The detectors read it. The race's lean reads two numbers in it
  (+640 and +644).
- **Fallback**: our own skeleton-based answer for the parts the game normally
  takes from the depth image, which a v2 does not give us.

## How a Kinect v2 reaches the game

```
Kinect20.dll (SDK 2.0)
  -> kinect_v2_win32.cpp        worker thread: 25 joints -> 20 v1 joints, smoothing
  -> nui_hooks.cpp sub_827707B0 the game's NuiSkeletonGetNextFrame
       menu steadying, levelling, two players, pause gestures
  -> the game's Kinect manager  fills the body record (original code)
  -> nui_race_hooks.cpp         race only: lean and crouch/jump fallback
  -> the game's detectors       original code, plus the fallback's answers
```

### 1. The sensor thread (`src/kinect_v2_win32.cpp`)

- `open_kinect_v2` loads `Kinect20.dll` at run time. No SDK is needed to build.
  The COM interfaces are declared by hand, in the SDK's vtable order.
- `KinectSensor::open` tries a Kinect v1 first, then the v2.
  The log says `NATIVE_KINECT started=1 model=Kinect v2 depth=0`.
- `WindowsKinectV2::run` polls `AcquireLatestFrame` every 3 ms. For each
  tracked body it reads 25 joints, the tracking id, and the two hand states
  (open, fist, lasso).
- `kinect_v2_body` (`src/kinect_sensor.cpp`) turns the 25 joints into the 20
  joints of a Kinect v1, which is what the game knows. v1's "shoulder centre"
  is v2's SpineShoulder (20), not its Neck. The body's centre is SpineBase.
  Fingertips and thumbs are kept only for the Stop Sign pause gesture.
- The floor plane's normal becomes the frame's "gravity" (a v2 has no
  accelerometer reading in this path).
- `KinectJointSmoother` (`src/kinect_smoothing.cpp`) smooths every joint with
  the filter Kinect v1's SDK offers (Holt double exponential; 0.5 / 0.5 / 0.5,
  jitter radius 5 cm, max deviation 4 cm). On by default; the launcher's
  "Smooth Kinect v2 tracking" turns it off (`SFR_KINECT_SMOOTH=0`).
- Every 5 seconds: `NATIVE_KINECT model=v2 frames=150 with_body=150 bodies=1`.

### 2. The skeleton hook (`src/nui_hooks.cpp`, `sub_827707B0`)

The game asks for a skeleton frame about 60 times a second. With a Kinect:

1. Take the newest frame, if a new one came (`kinect->next`).
2. Menus only: steady the hands (`KinectBodySteadying`,
   `src/kinect_stability.cpp`). A hand held still on a button does not move
   the cursor. The radius is the launcher's "Menu cursor steadying" (8 mm).
3. Drop the bodies when no frame came for 500 ms (`kinect_expire_frame`).
4. Keep at most two bodies, each in its own slot while it is tracked
   (`KinectPlayerSlots`).
5. Level the frame (`kinect_level`): turn all joints so that the floor is
   flat. The sensor usually looks up or down a bit; the logs show 11 degrees.
6. Write the frame header, the floor and each body into the game's
   `NUI_SKELETON_FRAME` (`write_kinect_body`), with the SDK's slot and id.
7. Watch the pause gestures (`pause_gesture.cpp`): the console's 45-degree
   left-arm pose, or the v2-only Stop Sign (open hand, palm to the sensor).
8. Every 3 seconds: `NATIVE_KINECT_BODY` with the hips, shoulders, head and
   hands of the first body.

### 3. The race (`src/nui_race_hooks.cpp`)

On the console the race takes two things from the depth image, not the
skeleton: the **lean** (body record +640 / +644, pixel counts on each side of
the player) and the **crouch**. A v2 gives no depth image here, so:

- `sub_82438930` (the Kinect manager's update, once a frame) runs a
  `CameraRaceMotion` (`src/camera_race_motion.h`) on the body record of each
  player (`sensor_motion`). It is the same motion logic as the webcam's.
  - **Calibration**: the first 0.6 s of an upright, still stance at the race
    start (`CAMERA_RACE_CALIBRATION ready=1`). A stance leaning more than 8
    degrees is not taken for 3 seconds (#132).
  - **Lean**: the roll of the shoulders over the hips, against the calibrated
    roll. 4 degrees starts, 30 degrees is full. It is written to +640 / +644
    as `1 + 3.5 x lean` (`write_camera_lean`; launcher "Lean sensitivity").
    `sub_82918418` writes it again just before the race reads it.
  - **Height**: the hips above a floor fixed at calibration. The floor is put
    0.75 m under the calibrated hips (for the default 15 cm crouch depth), so
    the standing height starts at 0.75.
  - **Crouch**: the hips below 80 % of the standing height (15 cm by
    default; launcher "Crouch depth") for 0.12 s.
  - **Jump**: rising out of a crouch, or a **hop**: the hips at least 6 cm
    above the standing height for two samples.
- The game's own detectors still run on the real skeleton
  (`RACE_DETECTOR` -> `finish_sensor_detector`). When the original says "no"
  and the fallback says "yes", the fallback answers for:
  - `822C8778` crouch and `822CB840` charge (result bits +4 `0x7000`);
  - `822C9050` jump (+4 `0x200`, direction -1 at +88 / +92).
  Side (`822CA6B0`) is only traced. All the other detectors (arms, throws,
  brake, kick, tricks, ...) are the game's own, reading the real skeleton.
- "On your Gear!" (`822B72E0`) measures the real body, as on the console.
- `KINECT_MOTION` lines, written always (at most 600 a run): each crouch and
  jump the fallback saw, and every 3 s the highest and lowest hips against
  the standing height.

## What the logs show, issue by issue

Five `game.log` files: v0.8.0 and v0.8.3 (#97), v0.8.3 (#132), v0.9.0 (#132,
a later comment), v0.10.0 (#165). All are Kinect v2 (`model=Kinect v2
depth=0`), smoothing on, one player, about 2.6 m from the sensor.

### #97: tracking and calibration

- Smoothing (v0.7.2) fixed the shaking steering; the player confirmed it.
- Jumps with both feet did not count until v0.8.0; crouch-jumps did not count
  in v0.8.3 because the player's crouches were 12-14 cm and the default
  needs 15 cm. v0.9.1 added the launcher's Calibrate button.
- Still open: the menu cursor shakes, and changing "Menu cursor steadying"
  (0, 8, 20, 30 mm) made no difference for the player. No log has the hands
  at more than one sample per 3 seconds, so the shaking cannot be measured
  yet. The new trace records them ten times a second.

### #132: relay, left steering, jumping, cursor

- Left steering (fixed in v0.9.0): a calibration taken while leaning 15
  degrees made that lean the centre.
- The v0.10.0 log shows no left/right difference: in races the lean reached
  -30 to +26 degrees both ways; standing, it rests at about +2 degrees.
- Relay: no log has a relay attempt (one tracking id per race, except one
  loss in the v0.10.0 log where the player left the view and came back,
  `NATIVE_KINECT_ENTER tracking_id=1621`). Not diagnosed.
- Cursor: as above.

### #165: steering, jump and cursor on v0.10.0

**Jumps.** The fallback saw 45 jumps and 15 crouches in two races. So the
player's movement is seen. Whether the game then jumped is not in the log:
the detector's answer is only written with the trace on. Two things in the
log were wrong in our code and are fixed (below):

1. **Double jumps.** Twice a crouch-and-spring gave a jump and, right after
   it, a hop (`jump height=0.8719` then `jump height=0.8929`;
   `jump height=0.7529` then `0.9065`). The rise was caught just below the hop
   height, so the hop was not latched. (The old lines have no time stamp, so
   "right after" is the next lines of the log; the new lines carry `t_ms=`.)
2. **The standing height crept up while hopping.** It went from 0.7589 to
   0.8376 m during a run of hops, while the stance between hops stayed where
   it was (the 3-second windows show the standing hips at 0.93-0.98 of the
   learnt height afterwards). Each hop's rise and landing taught the stance
   a few millimetres. With a stance 4-6 cm too high, a hop must go 10-12 cm
   above the real stance, and a crouch counts earlier. The v0.9.0 log shows
   the same (0.7575 to 0.7721 over four jumps).

Most of the 45 are real separate hops: they come in runs of 4-10 within a few
seconds, each with the hips 10-40 % above the stance. Our guess is that the
player hopped again and again because the game did not react.

**Best hypothesis for "jump does not work"**: the jump bit does reach the
game's jump detector when it asks, but the rider only acts on a jump in some
states (for example at a ramp, or after the crouch was held long enough to
charge). The pad's jump is always "crouch, then release", so the game always
sees a crouch first; a hop sends a jump without one. The trace's
`KINECT_GESTURE` and `CAMERA_RACE_ACTOR` lines will tell which.

**Speed 0.7.** `RACE_FRAMESKIP ... speed=0.75` means the race ran at 75 % of
real time. The log says it is not the Kinect path:

- The sensor thread kept 30 frames a second throughout
  (`NATIVE_KINECT frames=150` per 5 s).
- Slow stretches line up with heavy parts of the course (`NATIVE_FRAME_HEARTBEAT`
  draws 600-1400) and with `blocked=` rising: frames that were not even drawn
  still took longer than 16.7 ms (`undrawn_ms=9-17`). That is the game's own
  work being slow, on a laptop (i7-8850H, Quadro P2000, 3840x2160 window).
- The same player's v0.9.0 log had `undrawn_ms=2-8` and speed 0.88-0.99; no
  Kinect code changed between v0.9.0 and v0.10.0.
- Our per-frame Kinect work is a few small loops over 20 joints. The new
  `KINECT_COST` trace lines measure it directly.

Things to check on that laptop: the Kinect service also uses the CPU and the
GPU; the main game thread is pinned to CPU 0 (`MAIN_HOST_AFFINITY mask=0x1`),
where Windows often handles USB 3 interrupts; try `SFR_MAIN_AFFINITY=0`.

**Steering "unstable"**: nothing in the log shows a bias. The full lean is
4.5x the neutral value (the game's own maximum), reached at 30 degrees, so
small torso movements already steer a lot. The trace's `CAMERA_RACE_MOTION`
lines (`lean=` and `written_lean=` twice a second) will show how much it
moves while the player thinks they stand straight.

Twice in the v0.10.0 log the race's body record went away for one update in
the middle of a race (`NUI_RACE_SENSOR_LEAN active=0`, then `active=1` with
the same record). Each time the fallback starts over and calibrates again
(the next `KINECT_MOTION window` has `standing=0.75`). The 8-degree rule keeps
a lean out of that calibration, but nothing keeps a held crouch out of it: a
crouch held still for 0.6 s at that moment would become the "standing"
height. The old logs cannot show it (the standing height always starts at
0.75 by construction). In a trace, compare the `KINECT_TRACE hip=` height at
`CAMERA_RACE_CALIBRATION ready=1` with the hips' height before it.

**Pause**: the pause gesture fired three times in each of the last four logs
(`NUI_PAUSE_GESTURE pause`).
The player said pausing works; nobody has said whether these were meant.

## What changed in this pass

All in `src/camera_race_motion.h`, both with a unit test in
`tests/camera_race_motion_test.cpp` built from the #165 numbers:

1. A crouch-and-spring latches the hop, so rising on into the air is one
   jump, not two.
2. Over a fixed floor (a real Kinect) the standing height is only learnt
   while the hips are still (moving less than 1.5 cm a sample for 0.25 s) and
   at least 1 s after a jump. A taller stance held still is still learnt.
   The webcam path (no fixed floor) is unchanged.

And the trace:

3. The launcher's **Motion trace** switch (Motion input page, shown with
   Kinect or Motion camera chosen) sets `SFR_CAMERA_RACE_TRACE=1`.
4. With it on, new numeric lines (no images): `KINECT_TRACE` (ten a second),
   `KINECT_TRACE_JOINTS` (once a second), `KINECT_COST` (every 300 frames),
   and the race lines below now also cover a real Kinect. Lines that can
   repeat every frame are capped at 20-30 a second (`dropped=` counts the
   rest). About 2-3 KB a second, so a 3-minute race adds about 0.5 MB.
5. `KINECT_MOTION` lines now carry `generation=` (sensor frame number) and
   `t_ms=` (time), with or without the trace.

## How to capture a trace

1. Launcher -> **Motion input** -> Camera: **Kinect**.
2. Turn on **Motion trace** (below "Menu cursor steadying").
3. Start the game. Do the thing that goes wrong, once, on purpose: for
   example one race where you crouch-jump 3 times, hop 3 times, lean fully
   left and right, and stand still for 5 seconds; or 20 seconds of holding
   the cursor still on a menu button.
4. Close the game. Attach `game.log` (next to `FreeRidersRecompiled.exe`;
   zip it if GitHub refuses it), and say what you did and when.
5. Turn the switch off again: it only makes the log bigger.

### Reading the lines

| Line | When | What to look at |
| --- | --- | --- |
| `KINECT_TRACE` | every 3rd sensor frame | `racing=`, per body `hip/sc/sl/sr/hl/hr` = hips, shoulder centre, shoulders, hands (x,y,z m); `tracked=` hips,left hand,right hand (2 tracked, 1 inferred); `hand=` v2 hand state (2 open, 3 fist, 4 lasso); `steady_mm=` how far menu steadying moved each hand |
| `KINECT_TRACE_JOINTS` | once a second | all 20 joints of the first body, `x,y,z/state` |
| `KINECT_COST part=skeleton` | every 300 frames | time our skeleton hook took (µs): mean and longest |
| `KINECT_COST part=race` | every 300 race frames | time the race fallback took (µs) |
| `KINECT_MOTION` | each crouch/jump, every 3 s | `height`, `standing`, `ratio`; `generation`, `t_ms` |
| `CAMERA_RACE_MOTION` | twice a second, and on a jump | `lean` (-1..1), `written_lean` (the pair the race reads), `crouch`, `jump`, `height`, `standing` |
| `KINECT_GESTURE` | when a detector's answer changes; every jump | `address` (822c8778 crouch, 822cb840 charge, 822c9050 jump, 822ca6b0 Side), `original` (the game's own answer: 1 yes, 2 no, 0 unsure), `result` (after the fallback), `primary`/`secondary` result bits |
| `CAMERA_RACE_ACTOR` | when the rider's flags change | `flags` the rider read (0x200 jump, 0x7000 crouch), `mask1/mask2` what it allows, `anim`, `state`, `source=kinect` |

A jump that "does not work" then reads like this:

- `KINECT_MOTION jump` with no `KINECT_GESTURE address=0x822c9050 ... jump=1`
  near it: the game did not ask its jump detector at that moment.
- `KINECT_GESTURE ... 0x822c9050 original=2 result=1 jump=1`, then a
  `CAMERA_RACE_ACTOR` with `flags=` holding 0x200 but no change of `anim`:
  the rider got the jump and ignored it (state or ramp rule).
- `KINECT_GESTURE address=0x822ca6b0 result=1` (or `0`) around it: Side may
  be filtering crouch and jump. The pad and webcam paths answer Side with an
  explicit "no" (`secondary` bit 0x2000) unless the player brakes; the Kinect
  path leaves it to the game's own detector.

## What to test first with real hardware

In this order; each step only needs a trace and a few minutes.

1. **The trace itself**: one menu and one race with Motion trace on. Check
   that `KINECT_TRACE` lines appear, `KINECT_COST` means are in microseconds,
   and the log stays under a few MB.
2. **Menu cursor**: hold the right hand still on a button for 20 s, steadying
   at 8 mm, then at 0. From `KINECT_TRACE`, measure how far `hr` moves frame
   to frame, and `steady_mm`. If the raw hand jumps more than the radius,
   steadying cannot help; then look at the smoothing (its `prediction` of 0.5
   frames can overshoot when a hand stops), or the v2 hand joint flipping
   between tracked and inferred (`tracked=`).
3. **Jump**: crouch-jump three times on flat ground, then three times at a
   ramp. Compare `KINECT_GESTURE` (did 822c9050 say yes?) with
   `CAMERA_RACE_ACTOR` (did the rider take it?). Then hop with both feet the
   same way. This decides whether the fallback must send a crouch before a hop.
4. **Side** (`822ca6b0`): what the game's own detector answers on a v2
   skeleton while riding normally. The pad and webcam paths force "no Side"
   because a false "yes" suppresses crouch and jump; the Kinect path does not.
5. **Steering**: stand still 10 s, then lean slowly to full each way.
   `CAMERA_RACE_MOTION lean=` should stay 0 when still and reach +-1 at about
   30 degrees. If it wanders when still, the 4-degree dead zone is too small
   for a v2.
6. **Speed**: if a race slows down, compare `KINECT_COST` with the
   `RACE_FRAMESKIP undrawn_ms=`. Try `SFR_MAIN_AFFINITY=0`, and the race with
   the Kinect service stopped and a pad, on the same laptop.
7. **Relay**: a relay race where the first player walks fully out of view
   before the second steps in. Look for `NATIVE_KINECT_ENTER` with a new
   tracking id and `NUI_RACE_SENSOR_LEAN` switching records.
8. **COM order**: if `sfr_kinect_probe` sees no bodies on some machine, the
   hand-written vtables in `kinect_v2_win32.cpp` are the first suspect.

## Files

- `src/kinect_v2_win32.cpp`: the v2 sensor (COM, worker thread, hand states).
- `src/kinect_sensor.{h,cpp}`: v2 -> v1 joints, levelling, player slots, expiry.
- `src/kinect_smoothing.{h,cpp}`: the joint filter.
- `src/kinect_stability.{h,cpp}`, `src/pose_stability.h`: menu cursor steadying.
- `src/pause_gesture.{h,cpp}`: the 45-degree pose and the Stop Sign.
- `src/nui_hooks.cpp`: `sub_827707B0` (skeleton frames), the Kinect trace.
- `src/nui_race_hooks.cpp`: race lean, crouch/jump fallback, race trace.
- `src/camera_race_motion.h`: calibration, lean, crouch, jump, hop.
- `src/motion_trace.h`: the trace switch, rate limits, cost counters.
- `src/kinect_calibration.{h,cpp}`, `src/kinect_preview_window.cpp`: the
  launcher's preview and Calibrate.
- Tests: `kinect_sensor`, `kinect_smoothing`, `kinect_stability`,
  `camera_race_motion`, `nui_race_hooks`, `motion_trace`, `launcher_settings`.
