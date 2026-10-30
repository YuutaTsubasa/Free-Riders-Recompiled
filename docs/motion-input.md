# 體感輸入（Motion Input）

遊戲原本只認得 Xbox 360 的 Kinect。這個移植版讓好幾種裝置都能當那台 Kinect：
各平台的 webcam（加上姿勢模型），以及實體 Kinect v1／v2（Windows 走微軟 SDK，
macOS 走 libfreenect／libfreenect2）。這份文件說明它們怎麼共用同一套進入遊戲的路。

```
裝置（後端）                    統一的資料                      遊戲輸入對應
─────────────────────          ───────────────────            ─────────────────────────────
Camera＋姿勢模型 ───┐                                   ┌──▶ 感應器的身體：照主機上 Kinect 的
  Windows / Linux /  │                                  │     方式交給遊戲，遊戲自己的偵測器讀
  macOS / Android /  ├──▶ MotionFrame ＋ 能力 ──────────┤     （有深度圖時比賽也讀深度）
  iOS                │    （motion_input.h）            │
Kinect v1 / v2 ──────┘                                   └──▶ 一個身體：蓋在 1P 的手把模擬身體上；
  Windows SDK、                                                比賽透過攝影機橋接
  macOS libfreenect[2]                                         （camera_race_motion.h）
```

## 三層

1. **裝置（後端）**：只做三件事，打開、交出一格一格的身體、說明自己能做什麼。
   - 攝影機：[`CameraPlayer`](../src/camera_player.h)（擷取在 `camera_capture_*`，
     姿勢在 `pose_*`）。
   - Kinect：[`KinectSensor`](../src/kinect_sensor.h) 的各個實作（`kinect_sensor_win32.cpp`、
     `kinect_v2_win32.cpp`、`kinect_sensor_freenect.cpp`、`kinect_sensor_freenect2.cpp`）。
2. **統一的資料**：[`MotionInput`](../src/motion_input.h) 是唯一的入口。
   - 它依 launcher 的設定（`SFR_CAMERA`）選一個裝置，第一次被問到時打開，之後交出
     `MotionFrame`。
   - `MotionFrame` 就是 Kinect 的骨架格式：NUI 的骨架空間（公尺，+x 是玩家的右邊，
     +z 是離開裝置的方向）、20 個關節和各自的追蹤狀態。不管是哪種裝置，都是這個格式。
   - 攝影機的身體也包成這種格式：tracking id 1、slot 0、關節全部算「追蹤中」、中心在髖部
     （`motion_camera_body`）。
3. **遊戲輸入對應**：[`nui_hooks.cpp`](../src/nui_hooks.cpp)、
   [`nui_race_hooks.cpp`](../src/nui_race_hooks.cpp)。這一層只問裝置「能做什麼」
   （`MotionCapabilities`），不問「是哪種裝置」。

## 能力決定遊戲怎麼讀

| 能力 | 意思 | Kinect | 攝影機 |
| --- | --- | --- | --- |
| `sensor_bodies` | 身體是裝置自己的：各有 tracking id 和 slot，可以有好幾個，遊戲照主機上的方式認人、分 1P／2P、各自偵測暫停手勢 | 是 | 否：只有一個身體，在沒人碰手把時取代 1P |
| `depth_image` | 有深度＋玩家編號影像給遊戲自己的影像串流，比賽的偵測器讀它 | v1 有；v2 由 `kinect_v2_synthesize_depth` 做一張；深度開不了時沒有 | 否：比賽改走攝影機橋接 |
| `title_smoothing` | 由遊戲自己的 `NuiTransformSmooth` 平滑 | 是（除非 `SFR_KINECT_SMOOTH=1` 讓後端自己做） | 否：攝影機自己先平滑（`pose_smoothing.h`） |
| `max_bodies` | 一格最多幾個身體 | v1 2、v2 6 | 1 |

`nui_body_from_sensor()` 就是 `sensor_bodies`。比賽那邊（`nui_race_hooks.cpp`）原本就是依它
和 `camera_motion_active()` 決定走「遊戲自己的偵測器」還是「攝影機橋接」。

## 共用的部分

- **姿勢點 → NUI 關節**：攝影機（`pose_to_joints`）和 Mac 上的 Kinect（`kinect_depth_body`）
  都用姿勢模型找身體，再用同一份 [`pose_nui_joints`](../src/pose_nui_joints.h) 把 17 個點
  換成 20 個關節。兩者只差「點放在哪」：攝影機只看畫面，Kinect 把每個點放到深度圖上；
  Kinect 版另外把頭往後推 7 公分（模型看到的是鼻子）。
- **選單手部穩定**：兩邊都用 `MenuHandSteadying`（Kinect 在 `kinect_stability.h` 依
  tracking id 各包一份）。

## 加一種新裝置

1. 實作一個後端：Kinect 類（有自己的多個身體、可能有深度）就實作 `KinectSensor`；
   只看畫面的就走 `CameraCapture`＋姿勢模型。
2. 在 `MotionInput::start` 打開它，在 `motion_capabilities` 說明它的能力。
3. 遊戲那一側不用改：它依能力決定怎麼讀。

## 還沒統一的地方

- 調整視窗與 launcher 預覽：兩種裝置各有自己的視窗（`KinectPreviewWindow`、
  `CameraTunerWindow`），因為要調的東西不一樣。`MotionInput::kinect()`／`camera()` 就是給這些
  只有某種裝置才有的東西用的。
- 攝影機的身體在固定的虛擬空間（髖部在 2.5 公尺、肩寬 0.36 公尺），不是真實距離，
  所以還不能替它做深度圖；有深度的攝影機（例如手機的 LiDAR）之後可以補上。
- 攝影機只控制 1P；2P 一定是手把。
