# 架構遷移：往 Unleashed／Marathon Recompiled 的做法靠攏

2026-10-04。依據本機 `C:\Users\User\Repo\UnleashedRecomp`、`C:\Users\User\Repo\MarathonRecomp`
的原始碼，以及本專案（`claude/perf-v047`，`dfacc99`）的盤點。

## 兩邊的差異

| 面向 | Unleashed／Marathon | Free Riders Recompiled（現在） |
| --- | --- | --- |
| 記憶體 | 4 GB 一次 commit 在固定位址，`PPC_LOAD_*`／`PPC_STORE_*` 直接 `base + x`，只有第一頁不可存取 | 4 GB placeholder、逐段 commit；每次存取都經過 `GuestMemory` 的頁面檢查、reservation、寫入監看、特殊字 |
| lwarx／stwcx. | 比值的 CAS（容許 ABA） | 1024 個帶版本的 stripe + CAS（`docs/reservations.md`：只比值曾讓 Grand Prix 載入 R6025） |
| 執行緒 | 每個 guest 執行緒一個 `std::thread`，完全並行，沒有全域鎖；不設主機親和性 | 全域許可 + 每核心許可（`guest_execution.*`），檢查點、hook／import 回到許可 |
| 同步 | 事件、號誌用 `std::atomic::wait`；臨界區用 `atomic_ref` 直接在 guest 記憶體上 CAS | `NativeSyncObjects` 自己上鎖；臨界區自己上鎖；等待時放掉許可 |
| 配置器 | o1heap + mutex 取代遊戲的 `RtlAllocateHeap`、`XAllocMem` 等 | 遊戲自己的堆積（靜態連結）跑在 `NtAllocateVirtualMemory` 上；池配置器 `824A3398` 靠全域許可序列化 |
| 改遊戲 | 重編譯時的 mid-asm hook（Unleashed 191 個，含高幀率修正）、`GUEST_FUNCTION_HOOK`、`__imp__` 包裝 | 執行期 `SFR_HOOK`（70 個要拿許可）／`SFR_CONCURRENT_HOOK`（54 個） |
| Render target | 每個 guest surface 一張大小相符的主機貼圖；framebuffer 依（深度, 顏色）快取 | 只有一張 720p 顏色 + 深度；小畫布都畫在它上面（`guest_graphics_state.cpp` 的 viewport 註解） |
| Resolve | 延遲：被取樣時直接取來源 surface，只有來源要被覆寫前才複製（畫一個全畫面 copy shader） | 立刻把整張 720p 複製成一張 720p 貼圖，每次都打斷 render pass |
| 渲染執行緒 | 所有錄製在 render thread（無鎖佇列，32 個一批） | D3D12 與 Android 開 render thread，Windows Vulkan 關 |
| 時間 | QPC／`mftb` 對到主機時鐘；拿掉遊戲的 vsync 等待；delta time 修正 | 實時比賽／UI 時鐘（Android 預設） |

## 不能照搬的地方（本專案已經量過、不能退回的）

1. **Reservation 要保留 stripe 版本。** Unleashed 的「只比值 CAS」在這款遊戲上已證實會讓無鎖串列接上已釋放的節點（R6025）。直接記憶體之後仍要用 `generate_diagnostic.py` 的 reservation rewrite 與 stripe hook——它們本來就不需要檢查式記憶體。
2. **工作分派 `823B5D40` 的 16 ms 等待、執行緒「resume 後晚一點才開始」** 是遊戲本身的時序假設，跟許可無關，修正要保留。
3. **同一個 360 硬體執行緒上的兩條執行緒不能同時跑**（`SFR_PARALLEL_WORKER=all` 在比賽開始 R6025）。證據不算強，但在找到真正原因前保留「每核心許可」。
4. **ARM 的記憶體屏障 rewrite** 保留。
5. **池配置器 `824A3398` 並行時指標被破壞**：要嘛換成主機堆積，要嘛保留序列化。

## 分階段計畫（由效益高、風險可控的開始）

### 第 1 階段：每個 guest surface 一張 render target ＋ 延遲 resolve（渲染器）

- **為什麼先做：** Thor 的 GPU 每幀 38.5–39 ms，主因是約 20 個 110×90／55×45 的 bloom pass 被當成全畫面 720p 繪製與複製（`SFR_RESOLVE_STATS`、`SFR_GPU_TIMING`）。桌機 RTX 4090 只要 2.8 ms，所以只有手機吃得到這個效益，但它是 Android 的唯一上限。
- **做法（照 Marathon／Unleashed）：**
  1. 依 guest surface（EDRAM base、寬高、格式）建立主機 render target，大小依解析度縮放；framebuffer 依（顏色, 深度）快取。
  2. SetRenderTarget／SetDepthStencilSurface（`824E8FF0`、`824E9038`、`824E9E20`、`824E9B48`）切換目前的 framebuffer；viewport／scissor 改成相對於目前 surface。
  3. Resolve 先只記錄「目的貼圖 ← 來源 surface」；取樣時直接用來源 surface；來源要被清除或再畫之前才真的複製（含深度 resolve，取代現在的深度佔位貼圖）。
  4. 拿掉 `resolved_texture_scale` 與描述子最高位元的權宜做法（貼圖大小就是真實大小）。
- **驗證：** 桌機同一場景前後截圖逐像素比對；`SFR_GPU_TIMING` 在 Thor 與桌機；Thor A/B/A（第 15000–20500 幀）。
- **風險：** 中。全在渲染器內，不碰 CPU 端的正確性。
- **進度（2026-10-04）：** 第 1、2 項已做，`SFR_SURFACE_TARGETS=1` 開啟（預設關）。
  - 遊戲每個 pass 都設 0,0,1280,720 的 viewport（連 110×90、768×768 的 surface 也是），畫的是蓋滿 viewport 的四邊形；所以原點為 0 的 pass 把「虛擬 1280×720 畫面」映到整張 surface，有偏移的（分割畫面）用 surface 自己的像素。
  - 同一個 surface 物件會以不同大小重用（110×90／112×92／55×45），target 以（surface 指標, +36 大小字）為鍵；每張有自己的深度（bloom pass 的深度測試都是關的）。
  - Resolve 從目前的 target 複製到同大小的貼圖（第 3 項的「延遲／別名」還沒做）。
  - 結果：選單逐像素相同；比賽中舊路徑會隨時間變亮（曝光鏈 64→16→4→1 原本讀的是全解析度的一個像素，不是平均），新路徑聚光燈周圍較暗、對比較高——推測較接近主機，但尚未與主機或 Xenia 截圖比對。
  - AYN Thor（frames 15000–20500，A/B/A/B，同一個 APK 切換環境變數）：GPU 39.3／38.7 → 21.9／21.9 ms，fps 25.0／25.5 → 29.6／29.5。RTX 4090：6.7 → 5.8 ms。CTest 141/141。

### 第 2 階段：主機堆積取代遊戲的配置器

- **前置：** 找出遊戲靜態連結的 malloc／free／HeapAlloc 與池配置器 `824A3398`（及其 free）的位址與語意（盤點時 repo 裡還沒有這些位址）。
- **做法：** o1heap（或等價）＋ mutex，照 Unleashed 的 `kernel/heap.cpp`；池配置器整組換掉後就不需要全域許可。
- **效益：** 拿掉比賽開場時配置器持有全域許可 100–570 ms 的卡頓；也是第 4 階段的前置。
- **風險：** 中高（要完整掌握配置器的語意）。

### 第 3 階段：直接記憶體存取

- **做法：** 一次 commit 4 GB（或保留現在的方式但取消檢查），`PPC_LOAD`／`PPC_STORE` 回到上游的直接存取；向量、cache zero 的 rewrite 可回上游，update form、reservation、barrier 的 rewrite 保留。
- **必須先換掉的依賴：**
  - 頂點快取、貼圖快取靠「寫入監看」判斷是否失效 → 改成內容雜湊或 D3D Lock／Unlock、建立 buffer 的 hook。
  - 特殊字：`VdGlobalDevice`（有副作用，要改成 hook `824F19E8`）、`KeDebugMonitorData`、時間戳記（改成主機計時執行緒寫入）、其他 import 變數寫實值。
  - 非法存取會從 `RuntimeStop`（帶 guest 位址）變成主機 access violation → 加一個把主機位址換回 guest 位址的崩潰處理。
- **效益：** Thor 主執行緒約 12% 花在記憶體檢查與 helper。
- **風險：** 高（失去目前的除錯能力，快取失效錯誤會變成畫面錯誤而不是停止）。

### 第 4 階段：拿掉全域許可

- **前置：** 第 2、3 階段；PhysicalMemory、VirtualMemory、GuestGraphics／NativeRenderer、GuestFiles、GuestMemory 版面、GuestThreads 註冊表各自加鎖（現在都靠許可保護）；圖形 hook 只能由 guest 1 呼叫的假設要明文化或加鎖。
- **做法：** 先只拿掉全域許可、保留每核心許可（同一硬體執行緒不並行）；檢查點改成只負責取消／關機。
- **效益：** 主執行緒排隊（Thor 每幀 3–7 ms）與交接空窗。
- **風險：** 最高（R6025 類當機的歷史就在這裡）。

### 第 5 階段：重編譯期的 mid-asm hook 與幀率

- 在 `generate_diagnostic.py`／XenonRecomp 設定支援 mid-asm hook（Unleashed 的 TOML 格式），把能在重編譯期做的修補移過去。
- 拿掉遊戲的 vsync 等待、做 delta time 修正，支援 60 以上的幀率（與現有實時比賽時鐘整合）。

## 不打算照搬的

- Unleashed／Marathon 的事件只支援 0 與 INFINITE 逾時、`KeWaitForMultipleObjects` 只處理事件：本專案的 `NativeSyncObjects` 比它們完整，保留。
- Marathon 的 render target 不做解析度縮放（直接改遊戲的設定）：本專案保留 `SFR_RENDER_SCALE`。
