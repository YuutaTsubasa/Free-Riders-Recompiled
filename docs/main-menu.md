# 從標題畫面進入主選單

日期：2026-09-20。分支 `claude/function-boundaries`。

原生執行（D3D12）現在可以從開機一路走到主選單：標題影片 → 標題畫面「START」→
語音辨識語言提示 → Kinect 登入（沒有設定檔，選「Continue without saving」）→
「要讓 Omochao 教你怎麼玩嗎？」選 ✗ → **Main Menu**（Offline Mode 等模式圓環）。
選單以模擬的 Kinect 手部游標操作；現在也可以直接用手把按鍵（見 [pad-menus.md](pad-menus.md)）。

## Kinect 模擬

Sonic Free Riders 只接受 Kinect。遊戲靜態連結 Kinect SDK 的 NUI 執行庫；我們在
它的 API 層級接管（`src/nui_hooks.cpp`），並不模擬攝影機或深度影像：

| 位址 | 函式 | 行為 |
| --- | --- | --- |
| `8276FD88` | NuiInitialize | 成功 |
| `8276DF30` | NuiShutdown | 停止事件 |
| `82770668` | NuiSkeletonTrackingEnable | 以 30 Hz 觸發遊戲的 `Event_NuiGetSkeleton` |
| `8276FEE0` | NuiSkeletonTrackingDisable | 停止事件 |
| `827707B0` | NuiSkeletonGetNextFrame | 一位站在感測器前 2.5 m 的玩家 |
| `82764620` | NuiIdentityIdentify | 立即以「未註冊訪客」完成並呼叫遊戲的回呼 |

- 裝置狀態（`XamNuiGetDeviceStatus`）回報已連接（+12 為 3）。
- 骨架先以「尚未辨識」（註冊索引 -1）出現，遊戲的 Kinect 管理器（`82437E48`）才會
  啟動身分辨識；完成後改為訪客（-2），玩家才會被指派到 KinnectNuiBox 的 P1（+0x78）。
- 手把：第一次推右類比時，模擬的右手先舉高約 1 秒（遊戲要看到舉手才開始追蹤游標），
  再移到游標位於畫面中央的位置；之後右類比以速度移動游標（水平、垂直約每秒 900 px）。
  BACK 把手放下（游標消失，下次推類比重新舉手）；RB／LB 把手往前推；左類比對應左手。
- 選取方式與 Kinect 相同：游標停在按鈕上。START 幾乎一碰到就選取；對話框按鈕需要停留數秒。
- 影片：A、B、START、BACK 可跳過正在播放的影片（遊戲收到「影片結束」）；
  `SFR_SKIP_MOVIES=1` 讓每段影片播 30 格後自動結束，開機約 35 秒即到標題。

系統 UI（沒有 XAM 介面可顯示）：

- `XamShowNuiMessageBoxUI`：選遊戲指定的焦點按鈕並立即完成（XOVERLAPPED、事件）；
  因為沒有設定檔，「Sign in」按鈕會改選下一個。
- `XamShowNuiSigninUI`：發出 XN_SYS_UI 開啟／關閉，登入狀態不變。
- `XamShowNuiTroubleshooterUI`：回報已關閉。

線索來源：使用者提供的 No Kinect Patch（GameBanana，CC BY-NC-ND）中 Lua 腳本的
逆向筆記（KinnectNuiBox 與游標相關位址）。沒有使用或複製它的程式碼。

## 為了畫出選單而補上的圖形功能

| 項目 | 內容 |
| --- | --- |
| 索引繪製 | `824F56E8` DrawIndexedVertices：依索引收集頂點，primitive restart 的三角帶轉成三角形清單 |
| 頂點格式 | DEC3N（10:10:10 有號正規化）在 CPU 轉為 SNORM16，附加在頂點後 |
| 常數 | 浮點／布林／整數常數設定函式以原始程式寫入 device 影子區 |
| TEXCOORD4-7 | 釘選版 XenosRecomp 只對應 TEXCOORD0-3；翻譯前把容器中的 TEXCOORD4-6 改名為 POSITION1-3、TEXCOORD7 改為 NORMAL1，繪製端同樣綁定（快取世代 `v3-`）。手部游標等 3D 物件因此能畫出 |
| 貼圖 | `k_8_8_8_8`（XYZW／ZYXW swizzle）；Xenos 2D tiled 反交錯（區塊 ≥4 位元組） |

## 效能

主機端取樣器（`SFR_HOST_PROFILE`，配合連結器產生的 `sfr_cpu_diagnostic.map`）找出
客體記憶體檢查的熱點：write-combined 與被監看頁、dcbz、影片串流期間的所有寫入都走慢
路徑，動態貼圖每次使用都從不經快取的記憶體重新雜湊。修正後標題影片約 70 秒播完。

## 重現

```
set SFR_SKIP_MOVIES=1
set SFR_INPUT_AFTER_PRESENT=1800
set SFR_INPUT_SCRIPT=back@0+0.2,rright@1+0.03,rdown@3+0.19,rright@6+0.16,rup@6.5+0.05,back@12+0.2,...
```

每 12 秒重複一次「BACK→輕推右類比（舉手、游標到中央）→往下 0.19 秒到 START→往右 0.16 秒、往上 0.05 秒到 ✗」即可，搭配 `SFR_SKIP_MOVIES=1`、`SFR_INPUT_AFTER_PRESENT=1800`。
新增的除錯工具：`SFR_HOST_PROFILE`、`SFR_PRESENT_LIMIT`、`SFR_INPUT_AFTER_PRESENT`、
搖桿方向腳本（`rup`、`rleft`…）、`SFR_DUMP_OFFSET`／`SFR_DUMP_EVERY`、`SFR_TRACE_ENTRY`／`SFR_TRACE_RANGE`。

## 尚未完成

- A 鍵「立即選取」尚未完成：對話框與主選單的停留選取不在按鈕物件的 +552 計時器
  （那是每 10 幀的節拍），也不在選單處理器 +488 的延遲執行資料；遊戲內建的手把游標
  模式（`[0x83E52FB8]+5464`）需要把手把裝置綁定到玩家，目前讀不到資料。
- 沒有玩家設定檔，因此無法存檔。
- 部分頂點著色器仍無法翻譯（頂點取用超出宣告），相關 3D 物件不會畫出。
- 一位元組紋素的 tiled 貼圖、其他 swizzle 的 8888 格式尚未支援。
