# 從載入畫面到標題影片

日期：2026-09-20。分支 `claude/function-boundaries`。

## 目前可見的畫面

Windows 原生執行（D3D12）依序顯示：

1. 「Now Loading」與 Kinect 玩家圖示（DrawVerticesUP 原生繪製的精靈）。
2. ESRB 線上互動提示、SEGA、Sonic Team 標誌。
3. 白色淡出與長時間載入。
4. 標題影片 `game:\movie\titleMovie_j.wmv`：以「SONIC FREE RIDERS」標誌開始，
   遊戲自己的 WMV 解碼器（客體程式碼）逐幀寫入 Y/U/V 三個 k_8 平面，
   原生繪製以轉譯的 YUV→RGB 像素著色器呈現，畫面與 ffmpeg 參考幀一致。

尚未確認：從標題進入主選單（見下方「阻礙」）。

## 為了走到這裡而加入的機制

| 範圍 | 內容 |
| --- | --- |
| 排程 | 客體執行權改為 2 ms 時間片與無鎖快速檢查點；新建立的執行緒不搶先執行；time-critical 執行緒（優先權增量 ≥15）與音訊泵從等待醒來時排在一般執行緒之前，並讓一般擁有者在下個檢查點讓出 |
| 記憶體 | 每頁快速存取表（O(1) 檢查）、相鄰提交區段視為連續、寫入監視 |
| 核心 | 執行緒暫停計數與自我暫停、ExTerminateThread、等待執行緒控制代碼、多物件等待、檔案屬性查詢與定位、緩衝非同步開檔 |
| 音訊 | 系統執行緒每 256 樣本呼叫 XAudio 客戶端（影片時鐘依賴它）；等待執行權而落後的幀會補跑 |
| GPU | InsertCallback 的回呼在該幀 present 後於同一執行緒執行；每幀批次提交；64 MiB 上傳環 |
| 著色器 | 預備快取以外的著色器於執行期以釘選版 XenosRecomp＋DXC 轉譯並快取；無法轉譯者仍建立、繪製時略過並記錄 |
| 螢幕空間 | 停用 viewport 轉換（PA_CL_VTE_CNTL）時，頂點輸出由像素座標轉成裁切座標 |
| 貼圖 | 取樣依 fetch 常數（含 SetTexture(NULL) 後仍保留的常數）；CPU 改寫的貼圖以內容雜湊偵測後重新上傳 |
| 產生器 | bso/bns、缺失的 CR 更新、subfze.、cror/crorc、向量 pack、vslo、lvehx；0x82820BD8 設為單一函式；停用函式 140→23 |
| 遊戲修補 | 攝影機影像物件在串流開啟失敗（無 Kinect）時留下未初始化欄位，建構前先清零；影片以阻塞模式取下一格（見下節） |

## 影片黑幀與播放速度

症狀：標題影片一格有畫面、一格全黑地交替，而且播放很慢。

- 黑幀：遊戲每幀以旗標 bit0（無已解碼影格就立即返回）呼叫 XMV 播放器的
  RenderNextFrame（`82817B48` → 播放器 vtable +80 → `82827930`）。解碼佇列是空的時候，
  它不畫影片就返回，該幀只剩清成黑色的背景。實機上解碼執行緒有自己的核心，
  永遠有影格；這裡所有客體執行緒共用一個執行權，解碼跟不上，約三分之二的幀是黑的。
  修補 `82817B48` 清掉 bit0，讓播放器等待自己的「新影格／結束／錯誤」事件
  （`828266C8`），等待期間執行權也交給解碼器。之後每次 present 都有影片。
- 影片時鐘：XAudio 客戶端回呼（`8272AC40`）每 256 樣本喚醒混音執行緒並等它混好一格。
  混音執行緒是 time-critical，但先進先出的執行權讓它排在十幾個執行緒之後，
  音訊時鐘只有實際時間的 0.2～0.4 倍。加入上表的優先權排程後，音訊時鐘與實際時間一致。
- 仍然偏慢：解碼器（客體程式碼）目前每秒只產生約 2 格。剖析顯示熱點在
  `__savegprlr`／`__restgprlr` 這類極短函式，也就是每次函式呼叫的診斷執行期開銷
  （檢查點、記憶體檢查）；要接近 30 fps 需要降低這部分開銷。

## 除錯工具

`SFR_SCREENSHOT`（路徑含 `%d` 時每 60 次 present 一張；`SFR_SCREENSHOT_EVERY`／
`SFR_SCREENSHOT_SKIP` 調整間隔與起點）、`SFR_FRAME_TRACE`（每次繪製一行）、
`SFR_TRACE_ENTRY`／`SFR_TRACE_RANGE`（函式進入記錄）、`SFR_INPUT_SCRIPT`
（例如 `start@300+0.4,a@310`）、`SFR_SAMPLE_PROFILE`、`SFR_WATCH`、`SFR_DUMP_ENTRY`／
`SFR_DUMP_R3`、`SFR_DRAW_DUMP`／`SFR_DRAW_DUMP_SOURCE`、`SFR_TEXTURE_STATS`。

## 阻礙

Sonic Free Riders 是 Kinect 專用遊戲。沒有 Kinect 時 `XamNuiGetDeviceStatus`
回報未連接（與 Xenia 相同），遊戲從未呼叫 NuiInitialize。標題之後的操作是否接受
手把輸入仍待確認；若選單只接受 Kinect 手勢，需要模擬 NUI 裝置與骨架資料。
