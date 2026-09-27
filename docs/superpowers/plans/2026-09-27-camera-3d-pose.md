# Camera 3D pose upgrade

User explicitly chose a 3D pose model instead of simulated hand depth after the debug window revealed all RTMPose joints at fixed Z. Implement locally in codex/camera-debug; no PR/release requested.

Use OpenCV Zoo's Apache-2.0 MediaPipe person detector and pose model with existing ONNX Runtime, pinned downloads and hashes. Detector obtains a full-body rotated crop; pose outputs world landmarks in metres. Infer on the existing camera worker, single-thread sessions, one person. No camera image in the debug window. Relative joint Z is model-derived; body remains anchored at 2.5 m (monocular absolute distance is not claimed).

Interface: preserve the 17-point PoseLandmarks array and append `std::array<float,3> world{}; bool has_world=false;` to each PoseLandmark. MediaPipe's 33 body points map to existing COCO indices. Legacy SimCC models remain supported explicitly. World coordinates are image-oriented +X right/+Y down/+Z away; convert X/Y signs and body-centre offset to game space, retaining Z differences. Mirror handling must also transform world coordinates. Smooth world XYZ separately with metre-appropriate velocity scaling, reset on reacquisition.

- [x] Mapping agent: pose_estimator.h contract, pose_skeleton.cpp/.h, pose_smoothing.cpp/.h, corresponding tests. World coordinates, mirror, finite validation, depth preservation, smoothing tests first.
- [x] Parent: pure MediaPipe detector anchors/ROI, bilinear crop and landmark decoding, tests; ONNX model dispatch and tracking/reacquisition.
- [x] Parent: pinned model fetch, license/source notes, model defaults, accurate camera-hand diagnostic log.
- [x] Validate real model loading/inference against non-private test input, targeted regression tests and performance; independent review.
- [x] Rebuild and stage local test version; actual webcam cursor selection remains a user validation step.

References: https://github.com/opencv/opencv_zoo/tree/main/models/pose_estimation_mediapipe and sibling person_detection_mediapipe; https://developers.google.com/edge/mediapipe/solutions/vision/pose_landmarker/python .

## Verified result
- Real-model public-photo test: 30 tracked estimates, game Z range 2.27043..2.55851 m, ~11-12ms mean tracked estimate; blank-frame loss and reacquisition pass.
- Fixed tracked-crop shrinkage using the official 1.25 training margin; regression tests cover it.
- Windows 9/9 targeted CTest cases and 6/6 offline fetch tests pass. Linux 3 portable suites and ONNX source syntax checks pass.
- Independent review found no actionable 3D findings.
- Staged out/camera-3d-play at root; model/license hashes and executable hash verified. Executable-relative default model discovery tested without environment override.
- Camera-disabled Vulkan startup completed 180 presents and exited at the requested present limit.
- Live webcam reaching and cursor activation remain user validation; no user camera image recorded by these checks.
