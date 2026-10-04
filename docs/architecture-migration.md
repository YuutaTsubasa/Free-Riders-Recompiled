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
- **進度（2026-10-04）：** `SFR_HOST_HEAP=1`（預設關），`src/guest_heap.cpp`、`src/heap_hooks.cpp`。
  - 依呼叫結構辨認 XAPI 的 NT heap：`824E15E8` RtlAllocateHeap、`824E1EE0` RtlFreeHeap、`824E21D8` RtlReAllocateHeap、`824E0860` RtlSizeHeap、`824E01C8` RtlDestroyHeap（`824E1018` RtlCreateHeap 照舊）。XAllocMem `824DAD90`／XFreeMem `824DAEB0`／XMemSize `824DAED0` 的非實體路徑經由 HeapAlloc／HeapFree 走到這些 hook；實體路徑仍用 `MmAllocatePhysicalMemoryEx`（GPU 位址的視圖對應依賴它）。
  - o1heap 兩個 arena：`0x20400000–0x40000000`、`0x50000000–0x70000000`（約 1 GB），一把 mutex；hook 是 `SFR_CONCURRENT_HOOK`，不拿許可。
  - 與 Marathon 的差異：每個區塊記住它的 heap handle，`RtlDestroyHeap` 會一起釋放（遊戲有 6 處 HeapDestroy，Marathon 的做法會洩漏）。
  - 桌機整場比賽（三圈到結算）正常。池配置器 `824A3398` 尚未處理。

### 第 3 階段：直接記憶體存取

- **做法：** 一次 commit 4 GB（或保留現在的方式但取消檢查），`PPC_LOAD`／`PPC_STORE` 回到上游的直接存取；向量、cache zero 的 rewrite 可回上游，update form、reservation、barrier 的 rewrite 保留。
- **必須先換掉的依賴：**
  - 頂點快取、貼圖快取靠「寫入監看」判斷是否失效 → 改成內容雜湊或 D3D Lock／Unlock、建立 buffer 的 hook。
  - 特殊字：`VdGlobalDevice`（有副作用，要改成 hook `824F19E8`）、`KeDebugMonitorData`、時間戳記（改成主機計時執行緒寫入）、其他 import 變數寫實值。
  - 非法存取會從 `RuntimeStop`（帶 guest 位址）變成主機 access violation → 加一個把主機位址換回 guest 位址的崩潰處理。
- **效益：** Thor 主執行緒約 12% 花在記憶體檢查與 helper。
- **風險：** 高（失去目前的除錯能力，快取失效錯誤會變成畫面錯誤而不是停止）。
- **進度（2026-10-04）：** CMake `-DSFR_DIRECT_MEMORY=ON`（預設關）。
  - `PPC_LOAD_*`／`PPC_STORE_*` 是 `bswap(*(volatile T*)(base + uint32_t(x)))`，向量存取與 `dcbz` 也直接做；reservation、更新形式、屏障的 rewrite 不變。
  - 寫入監看改用頁面保護（`GuestMemory::direct_guest_access`）：被監看的頁面唯讀，第一次寫入時例外處理記錄並打開；打開的頁面視為已寫入，直到再被監看。Windows 用 vectored handler，Android／Linux 用 `SIGSEGV`（串接原本的 handler）。不在監看範圍的 guest 位址錯誤會印 `GUEST_ACCESS_VIOLATION guest=0x…`。
  - 每幀都被改寫的頁面（GPU 讀的環狀緩衝區）在 120 幀內第二次觸發就 300 幀不再上鎖；頂點快取同一項目連續三次被改寫也停止監看 300 幀。移動中的比賽每幀約 9.5 次例外（沒有這兩條規則時約 57 次）。
  - 特殊字：常數 provider 在註冊時寫入值；`VdGlobalDevice` 寫入 device cell（provider 的副作用只是診斷記錄）；`KeTimeStampBundle` 的 uptime 由主機執行緒每毫秒寫入。
  - AYN Thor（surface targets 開，A/B/A/B）：29.6／29.5 → 40.6／40.0 fps，主執行緒 CPU 22.5 → 17.4 ms。桌機整場比賽到結算畫面正確。

### 第 4 階段：拿掉全域許可

- **前置：** 第 2、3 階段；PhysicalMemory、VirtualMemory、GuestGraphics／NativeRenderer、GuestFiles、GuestMemory 版面、GuestThreads 註冊表各自加鎖（現在都靠許可保護）；圖形 hook 只能由 guest 1 呼叫的假設要明文化或加鎖。
- **做法：** 先只拿掉全域許可、保留每核心許可（同一硬體執行緒不並行）；檢查點改成只負責取消／關機。
- **效益：** 主執行緒排隊（Thor 每幀 3–7 ms）與交接空窗。
- **風險：** 最高（R6025 類當機的歷史就在這裡）。
- **進度（2026-10-04）：** 第一步 `SFR_PARALLEL_MAIN=1`（需要 `SFR_PARALLEL_WORKER=cores`，預設關）。
  - 處理器 0 的執行緒（含主執行緒）也只拿核心 0 的許可跑 guest 程式碼；全域許可只在 hook 與需要主機狀態的 import 時才拿。
  - 陷阱：開機時主執行緒用「執行一次」的執行緒函式（`824B2320`）開輔助執行緒跑 `sub_8222CD98`，在喚醒它們之後才把自己堆疊上的資料填好；以前全域許可讓它們在主執行緒阻塞前跑不起來。現在新執行緒一律等喚醒者阻塞一次（原有的 `SFR_RESUMER_WAIT_WORKERS=all` 機制），開機時間沒有變長。
  - 比賽中圖形 hook（`824F4220` 等）只有 guest 1 呼叫；池配置器 `824A3398` 主要是 guest 16。
  - 圖形鎖（`SFR_GRAPHICS_HOOK`／`SFR_GRAPHICS_HOST_HOOK`）：開 `SFR_PARALLEL_MAIN` 時，Direct3D 的 hook 拿圖形鎖；只碰原生裝置與 guest 記憶體的不再拿全域許可。建立／重設裝置與 present 兩者都拿，順序固定「圖形→全域」；拿著全域許可卻等不到圖形鎖時，先放掉全域許可再等。renderer 的 `invalidate`（釋放記憶體的 import）也拿圖形鎖。
  - Thor 上的陷阱：只拿圖形鎖的 hook 裡等 GPU 時，原本的包裝會放掉「沒拿著的」全域許可而中止；改成只放核心。中止後拆除時又因為對已送出的指令清單再 end 一次而讓 Adreno 驅動崩潰、蓋掉原因；已修，失敗也改成當下就印出（`EXECUTION_FAILURE`）。
  - `SFR_PARALLEL_WORKER=all` 加 `SFR_PARALLEL_MAIN=1`：所有 guest 執行緒（含主執行緒與音訊泵）自由執行，全域許可只剩 import 與非圖形 hook 的鎖——即 Unleashed 的模型。桌機與 Thor 都能跑完比賽；以前 `all` 模式的 R6025 其實是執行緒啟動競爭，已由「等喚醒者阻塞」解決。已知問題：`all` 模式偶有 guest 11（worker `824395E8`）持有全域許可 100–250 ms 的長幀，原因未明。
  - 並行執行緒每次進函式都走非內聯的觀察路徑；改成「不是 hook、也不在 hook 裡」時走內聯快速路徑（Thor 上沒有可量到的差別）。
  - `all` 模式長幀的真正原因：脫離的執行緒在 hook 入口拿了全域許可，要等之後進入一個堆疊更高的函式才放；Kinect 骨架執行緒在同一層迴圈裡呼叫 `NuiSkeletonGetNextFrame`，第一次之後就一直拿著，整個遊戲跟著它的時間片停頓。hook 現在包在 `HookScope` 裡，返回時就放掉全域許可與圖形鎖（`SFR_PERMIT_HOLD_TRACE=1` 會列出 20 ms 以上的持有）。同時：頁面保護改成連續頁一次設定、監看鎖每 64 頁放開一次；等待執行緒 handle 改由執行緒表自己的讀寫鎖查詢，不再拿全域許可。
  - 結果（Thor，同一個 APK，A/B/A/B）：`cores` 46.3／44.4 fps、`all` 46.4／46.3 fps（之前兩者都是約 41 fps）。**`all` 已是預設**（啟動器、Android、play 腳本），每個 guest 執行緒自由執行；全域許可只剩 import 與碰到主機狀態的 hook 的鎖。
  - 尚未做到的：把這把鎖再拆成各子系統自己的鎖（Unleashed 的做法），例如 NUI hook 仍偶有 20–30 ms 的持有。

### 第 5 階段：重編譯期的 mid-asm hook 與幀率

- 在 `generate_diagnostic.py`／XenonRecomp 設定支援 mid-asm hook（Unleashed 的 TOML 格式），把能在重編譯期做的修補移過去。
- 拿掉遊戲的 vsync 等待、做 delta time 修正，支援 60 以上的幀率（與現有實時比賽時鐘整合）。
- **進度（2026-10-04）：** `config/freeriders.toml` 的 `[[midasm_hook]]`（XenonRecomp／Unleashed 的格式）會經由 `prepare_recomp.py` 寫進 `recomp.toml`，`generate_diagnostic.py` 保留 XenonRecomp 輸出的 hook 宣告與呼叫。
  - 重新產生可重現：沒有 hook 時輸出和目前使用的生成程式碼逐位元組相同；加入 hook 後只有對應的檔案改變。
  - 第一組：工作分派等待（`823B5D40` 在 `0x823B5E9C` 呼叫 `sub_824D0B10`）改成 `WorkShareWaitMidAsmHook`（`0x823B5E98`，改 r6）與 `WorkShareWaitDoneMidAsmHook`（`0x823B5E9C` 之後，看 r3），取代用 LR 判斷的整函式 hook；舊的生成程式碼仍走原本的 hook。
  - 幀率：比賽與 UI 已用實時時鐘（`race_frame_clock_hooks.cpp`），幀率上限是主機端的 `SFR_FRAME_LIMIT`。

## 不打算照搬的

- Unleashed／Marathon 的事件只支援 0 與 INFINITE 逾時、`KeWaitForMultipleObjects` 只處理事件：本專案的 `NativeSyncObjects` 比它們完整，保留。
- Marathon 的 render target 不做解析度縮放（直接改遊戲的設定）：本專案保留 `SFR_RENDER_SCALE`。
