# 實體 Kinect

日期：2026-09-27。接續 [攝影機體感輸入](camera-input.md)。

遊戲原本就是 Kinect 專用的：Xbox 360 上的 NUI 函式庫回報玩家骨架，遊戲自己的
約 25 個手勢判斷器（跳躍、蹲下、踢地、傾斜……）讀這副骨架。沒有感測器時，
[NUI 模擬](main-menu.md) 用手把假裝出一副骨架，[比賽](race-controls.md) 更直接
把判斷器換成讀手把。

**Kinect for Windows 執行階段（SDK 1.8，`Kinect10.dll`）就是同一套 NUI 函式庫
搬到 PC 上**：骨架框架一樣是六副、每副 20 個關節、同一個攝影機座標系（公尺，
+x 朝玩家的右手邊，+z 離開感測器）。所以感測器看到的東西幾乎原封不動交給遊戲，
比賽也改回由遊戲自己的判斷器讀真實身體，而不是手把。

## 怎麼用

1. 需要一台 **Kinect for Xbox 360**（加上 USB 電源轉接線）或 **Kinect for Windows 第一代**。
2. 安裝 **Kinect for Windows SDK 1.8**（Microsoft 官方，含驅動程式與執行階段）。
   - 只有 Kinect for Windows 感測器時，較小的 *Kinect for Windows Runtime 1.8*
     就夠了；**Xbox 360 版的 Kinect 需要完整 SDK**，只裝 Runtime 會被拒絕。
3. launcher → 進階 →「攝影機」選 **Kinect**，按旁邊的「測試」確認有回應。
4. 開始遊戲。log 會有 `NATIVE_KINECT started=1`，之後每五秒一行
   `NATIVE_KINECT frames=… with_body=… bodies=…`。

不開遊戲也可以檢查：`sfr_kinect_probe [秒數]` 會印出追蹤到的人與右手位置。

找不到感測器或執行階段時（`NATIVE_KINECT started=0 reason=…`），遊戲會退回手把
模擬，不會卡住。`reason` 可能是：

| reason | 意思 |
| --- | --- |
| `no-runtime` | 沒有 `Kinect10.dll`：還沒裝 SDK／Runtime |
| `no-sensor` | 執行階段看不到感測器：沒插、沒接電源，或驅動程式沒裝好 |
| `initialize-0x…` | 感測器在但無法啟動：被別的程式佔用，或 Xbox 360 版只裝了 Runtime |
| `unsupported-platform` | 非 Windows（見下方） |

## 做了什麼

- [`kinect_sensor_win32.cpp`](../src/kinect_sensor_win32.cpp)：執行時才 `LoadLibrary`
  載入 `Kinect10.dll`（建置不需要 SDK，沒有 Kinect 的人也不需要那個 DLL），
  開啟骨架追蹤，在自己的執行緒上等感測器的新框架事件，套用 SDK 建議的
  `NuiTransformSmooth` 平滑參數（約一格延遲）。
- [`KinectPlayerSlots`](../src/kinect_sensor.h)：感測器最多完整追蹤兩個人。
  每個人只要還被追蹤就留在原本的欄位（遊戲以欄位與 tracking id 1／2 認人，
  第 1 位是登入的設定檔），新走進來的人補空的欄位。
- [`nui_hooks.cpp`](../src/nui_hooks.cpp) 的 `NuiSkeletonGetNextFrame`：
  `SFR_CAMERA=kinect` 時，框架表頭的地板平面與重力方向用感測器的，兩個欄位寫入
  真實的關節、每個關節的追蹤狀態（追蹤／推測）與身體中心。沒有人站在前面就是空框架，
  遊戲會像在主機上一樣請玩家站到感測器前。
- [`nui_race_hooks.cpp`](../src/nui_race_hooks.cpp)：有實體感測器時
  （`nui_body_from_sensor()`），比賽**不**換掉身體紀錄、**不**覆寫手勢判斷器，
  也**不**跳過「On your Gear!」的身體量測——全部交還給遊戲原本的程式。
- 語音指令仍由手把按鍵代替（START／A／B…），見 [用手把操作選單](pad-menus.md)。

## 其他平台

- **Linux**：Kinect 第一代沒有骨架追蹤器可用，但核心的 `gspca_kinect` 驅動會把
  它的彩色鏡頭變成一般的 `/dev/videoN`，可以用「體感」（webcam）模式讀它。
- **Kinect v2（Xbox One 版）**：尚未支援（需要 `Kinect20.dll` 的 COM 介面，25 個
  關節要對應到 20 個）。

## 還沒驗證的

這台開發機沒有 Kinect，也沒有遊戲光碟：資料結構大小由編譯期檢查確認
（`NUI_SKELETON_DATA` 436 bytes、`NUI_SKELETON_FRAME` 2664 bytes），欄位分配與
框架寫入有單元測試（`kinect_sensor` CTest），但**實機遊玩還沒試過**。
特別需要確認的：

- 選單的手部游標位置是否對得上（模擬玩家的右手在 x=0.175 為畫面中央）。
- 比賽中遊戲自己的判斷器在真實骨架上的反應，以及「On your Gear!」量測能否完成。
