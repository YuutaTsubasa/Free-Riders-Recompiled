# Android 效能工作交接給 Claude（2026-09-25）

這份文件供 Claude 整理 commit / push，並作為後續各自 worktree 的共同基準。
使用者目前掌機沒電，請以本機程式碼和既有實測資料接手。本次整理沒有再操作裝置、
沒有 commit、push、切換分支或建立 worktree。以下實機結果都是先前已完成的量測，
不是在掌機沒電後重新驗證的結果。

## 目前工作區與提交邊界

- 路徑：`C:/Users/User/Repo/FreeRidersRecompiled`
- 分支：`claude/camera-input`
- HEAD：`e9f5fcd5e87d41909b9449403ce2700080dd7652`
- 已提交的 `e9f5fcd` 是 Claude 的 TLS 整併／hook bitset；`bb463d8` 是前一輪 Adreno
  畫面與核心並行相關交付。這些已在歷史中，不需重複提交。
- 下列 round3–7 都仍在 working tree，還包含 Claude 原有的輸入 permit-free 修改。
  請保留這些共同成果，不要只提交最後一輪 pipeline key。
- 已存在另一個 worktree：`C:/Users/User/Repo/frr-ci`，分支 `main-push`，HEAD
  `bd915c243067c2497ae8c6841fcc5ac52668d6bc`。本次未修改它，勿當成可任意重用的空目錄。

## 已完成哪些事

| 工作 | 實際變更／結果 | 主要檔案 |
| --- | --- | --- |
| 保留 Claude 的輸入改動 | 三個 input import 不取全域許可；NativeInput 的 packet 狀態加鎖。先前報告未證明 FPS 改善。 | `src/native_input.{h,cpp}`、`src/diagnostic_main.cpp` |
| round3：修正許可量測 | 全域與六個核心各自統計持有／主執行緒 ready 時間，加入 guest→host TID 對應。Present 整行輸出與 frame 編號讓測量能拒絕不連續視窗。 | `src/guest_execution.{h,cpp}`、`src/guest_graphics_hooks.cpp`、`src/diagnostic_main.cpp`，相關 tests |
| round3：核心-only 等待實驗 | 新增 `Lease::run_wait` 與測試；沒有可靠 FPS 改善證據，預設仍用原本 global attach 政策。沒有移除全域許可或記憶體檢查。 | 同上及 `tests/guest_critical_sections_test.cpp` |
| round4：開場漏喚醒修正 | 特定 worker 在 SetEvent 前先登記即將 self-suspend，避免主執行緒的 ResumeThread 早到而遺失。連續三次冷啟動通過，另有 906 個比賽格無停止。 | `src/game_patches.cpp`、`src/guest_threads.{h,cpp}`、`src/diagnostic_main.cpp`、`tests/guest_threads_test.cpp` |
| round5：Vulkan 持久化管線快取 | 共用 VkPipelineCache，背景每 10 秒有新資料才保存；檢查檔案完整性、大小、vendor/device/UUID，原子替換。明顯減少編譯停頓。 | `src/native_pipeline_cache.{h,cpp}`、`src/pipeline_cache_file.{h,cpp}`、`src/native_graphics.cpp`、dependency patch、CMake、test |
| round6：成功等待日誌遵守開關 | 兩處未遵守 TRACE_IMPORTS 的高頻 RESULT 改為可關閉；錯誤狀態保留。日誌少約 87%，四趟未見穩定 FPS 改善。 | `src/diagnostic_main.cpp` |
| round6：按執行緒 CPU 取樣 | 12,526 samples、零遺失；定位主執行緒 draw 路徑、guest 29 呼叫鏈，以及 CAS 的 library lock 來源。 | 分析結果在 `docs/performance.md`，原始檔在 `out/android-logging-round6/` |
| round7：一次填入 pipeline key | 每 draw 上百次 vector insert 改成一次 resize 後逐欄位 memcpy。新舊 key 逐 byte 相同；本組四趟約 14.67→15.48 FPS。 | `src/native_renderer.cpp`、`src/native_pipeline_key.h`、`tests/native_pipeline_key_test.cpp`、CMake |

round4 只攔截 LR `0x824C3A3C` 的特定完成通知／自我暫停配對，不是把所有
ResumeThread 改成會累積的喚醒。另讓等待 callback 在放開許可之前取得穩定 record，
使用 atomic suspend count，避免在許可外遍歷可變 registry／讀取非 atomic 計數。
round3、round4 在同一批檔案有交疊；拆 commit 時應按 hunk 和依賴檢查。

## 效能證據與限制

### 管線快取（round5，舊／首次空快取／暖快取／再切回舊）

- 第二測量視窗 FPS：11.43 / 13.71 / 14.23 / 11.38。
- 該視窗 pipeline 建立累計：2022.84 / 616.46 / 9.41 / 2077.92 ms。
- 最慢一格：約 1189 / 344 / 160 / 1233 ms。
- 明確成果是重複編譯的大停頓縮短；新賽道／新狀態仍可能首次編譯。

### Pipeline key（round7，同 APK 原／新／新／原）

| 模式 | 100–200 FPS | 200–300 FPS | draw_ms 平均（兩段） |
| --- | ---: | ---: | --- |
| legacy-a | 14.83 | 14.51 | 9.902 / 11.749 |
| bulk-a | 15.83 | 14.65 | 8.844 / 10.206 |
| bulk-b | 16.32 | 15.22 | 8.370 / 9.900 |
| legacy-b | 15.60 | 13.86 | 9.185 / 10.912 |

每模式合併 400 個 interval，以總格數除總秒數，原 14.6746、新 15.4799 FPS，
約 +5.5%；draw_ms 平均 10.4369→9.3301 ms。所有視窗 frame 連續、電池起迄均
36°C、同一暖快取，兩趟新版的兩段都高於兩趟原方式。但沒有固定 gameplay replay、
SoC 頻率鎖定或 SoC 溫控，draws 第二段中位數仍有 792–793 差異。這是本組場景
的小幅改善，不能當成所有賽道保證；仍只有約 15 FPS，沒有達到流暢或 30 FPS。

`draw_ms` 包含 `record_ms`、constants、texture 等分項，不可重複相加。
CPU inclusive call-stack 比例同樣互相包含，且 CPU 樣本不包含 blocked time。
不要用 guest 29 的 core hold 當成它握住全域許可的時間。

## 正確性與已做驗證

以下是各輪已完成的驗證；這次交接只核對本機狀態與文件，未重跑全部測試：

- round3：Windows 的 guest_execution、guest_critical_sections、guest_memory、
  pending_guest_write、guest_threads、guest_wait、async_completion_primitives 七項通過；
  Android 的排程／critical-section 測試通過。涵蓋 live snapshot、scope 隔離、
  ready 歸因、detached 同核心 peer 前進、取消及真實 critical-section 交接。
- round4：先重現 early-resume 測試失敗，修正後通過；涵蓋早／晚 resume、一般
  self-suspend、外部巢狀暫停、重複登記、輸出位址錯誤、取消。Windows 六項相關
  回歸與 Android guest_threads 通過；實機三次冷啟動及比賽通過。
- round5：Windows／Android cache file 測試通過，含 round trip、覆寫、截斷、
  損壞、大小限制、不相容 header、替換失敗保留原檔。Host 五項回歸通過；
  D3D12／Vulkan graphics copy 及 Vulkan presentation clear/readback 通過。
  `python scripts/bootstrap.py --verify-only` 當時驗證 Plume 兩個 patch 可重現。
- round6：四趟 same-APK log on/off/off/on；成功抑制指定日誌，無 runtime stop，
  沒有把結果包裝成 FPS 改善；正式交付移除了 profileable。
- round7：先以空 bulk serializer 讓測試失敗，再通過 Windows／Android。
  重編後 host 的 native_pipeline_key、guest_graphics、guest_render_state、
  guest_blend_request 四項通過。實機超過 140 萬次 key 比對一致、456 個驗證
  比賽格；另四趟計時各 436 / 442 / 440 / 425 個比賽格，均無停止。
- 最後 `git diff --check` 通過，只有工作區既有 LF/CRLF 提示。

## 請 Claude 整理 commit / push 時留意

可以做一個完整整合 commit，或依序拆成「input／許可量測」「startup handoff」
「Vulkan cache」「quiet wait logging」「bulk pipeline key」與對應文件。
若拆分，不要單純按整個檔案分：diagnostic_main、guest_threads、CMake 和 performance.md
跨越多輪；拆出的各 commit 應保有其必要宣告、tests、CMake 與 dependency patch。

特別容易漏掉的新檔：

- `patches/plume-pipeline-cache.patch` 必須和 `config/dependencies.lock.json` 一起進 Git。
  `tools/Plume/` 被忽略，不能只靠本機直接改過的 dependency；新 worktree 必須可套 patch 重建。
- `src/native_pipeline_cache.{h,cpp}`、`src/pipeline_cache_file.{h,cpp}`、
  `tests/pipeline_cache_file_test.cpp`。
- `src/native_pipeline_key.h`、`tests/native_pipeline_key_test.cpp`。
- 五份 `docs/superpowers/plans/2026-09-25-android-*.md`（permit-measurement、
  startup-handoff、pipeline-cache、wait-logging、pipeline-key）及本交接文件。
- `.gitignore` 新增 `/pipeline-cache/`，`CMakeLists.txt` 的來源與 test target 也要保留。

`out/`、`android/app/src/main/jniLibs/`、`tools/Plume/`、遊戲素材和 cache 都不隨
普通 commit 傳送；不要為了交接把 APK、927 MB 的符號檔、遊戲素材或整個 out 強制加入 Git。
若需要異機重做分析，另保存下面的 evidence 目錄。commit / push 前請依當時分支、
remote、upstream 核對目的地；本文件沒有替使用者選擇或更改 push 目標。

### 整理開始前的 working-tree 清單

```text
 M .gitignore
 M CMakeLists.txt
 M config/dependencies.lock.json
 M docs/performance.md
 M src/diagnostic_main.cpp
 M src/game_patches.cpp
 M src/guest_execution.cpp
 M src/guest_execution.h
 M src/guest_graphics_hooks.cpp
 M src/guest_threads.cpp
 M src/guest_threads.h
 M src/native_graphics.cpp
 M src/native_input.cpp
 M src/native_input.h
 M src/native_renderer.cpp
 M tests/guest_critical_sections_test.cpp
 M tests/guest_execution_test.cpp
 M tests/guest_threads_test.cpp
?? docs/superpowers/plans/2026-09-25-android-permit-measurement.md
?? docs/superpowers/plans/2026-09-25-android-pipeline-cache.md
?? docs/superpowers/plans/2026-09-25-android-pipeline-key.md
?? docs/superpowers/plans/2026-09-25-android-startup-handoff.md
?? docs/superpowers/plans/2026-09-25-android-wait-logging.md
?? patches/plume-pipeline-cache.patch
?? src/native_pipeline_cache.cpp
?? src/native_pipeline_cache.h
?? src/native_pipeline_key.h
?? src/pipeline_cache_file.cpp
?? src/pipeline_cache_file.h
?? tests/native_pipeline_key_test.cpp
?? tests/pipeline_cache_file_test.cpp

```

本清單是在新增本文件之前取得，另外應納入本文件本身。

## 本機重建、分析與掌機狀態

- Host build：`out/build/host`；Android：`out/build/android-tls29`，Release / arm64 / minSdk 29。
- CMake／CTest：`C:/msys64/mingw64/bin/cmake.exe`、`ctest.exe`。
- **兩個 CMake cache 使用 SDK Ninja**：`C:/Users/User/AppData/Local/Android/Sdk/cmake/3.22.1/bin/ninja.exe`。
  之前 MSYS Ninja 曾遇到依賴檔異常，不要把這個已工作的設定改回去。
- NDK：`C:/Users/User/AppData/Local/Android/Sdk/ndk/29.0.13599879`。
- adb：`C:/Users/User/AppData/Local/Android/Sdk/platform-tools/adb.exe`。
- 裝置：AYANEO Pocket S2 Pro、Android 14、Adreno 750；serial `01005WHD11010887`。
- app：`com.freeriders.recompiled`，launcher `.LauncherActivity`，遊戲 process 帶 `:game`。

既有 build 目錄可用以下指令重建（新 worktree 必須重新 configure）：

```powershell
& C:/msys64/mingw64/bin/cmake.exe --build out/build/android-tls29 --target sfr_cpu_diagnostic sfr_native_pipeline_key_test -j16
& C:/msys64/mingw64/bin/cmake.exe --build out/build/host --target sfr_native_pipeline_key_test sfr_guest_graphics_test sfr_guest_render_state_test sfr_guest_blend_request_test -j8
& C:/msys64/mingw64/bin/ctest.exe --test-dir out/build/host -R '^(native_pipeline_key|guest_graphics|guest_render_state|guest_blend_request)$' --output-on-failure
python scripts/bootstrap.py --verify-only
```

封裝前用 NDK llvm-strip 把新 `out/build/android-tls29/libmain.so` 寫到
`android/app/src/main/jniLibs/arm64-v8a/libmain.so`，再執行：

```powershell
python scripts/package_android.py --abi arm64-v8a --min-sdk 29 --pack out/shaders/shaders.pack --output out/android/FreeRidersRecompiled.apk
```

最新已安裝且完成交付的 APK：`out/android-pipeline-key-round7/pipeline-key.apk`，
無 profileable。libmain.so SHA-256：
`3146937aa0ae158293975a19a87834e18169c08a60a1a57ece4e53c1e9465c17`。
Shader pack 自圖形修正後保持：
`d430c0c0bff5f7b9937a16d67dd3b5059bc9165296a440a73b32a9201fd93b4a`。
最後正常啟動確認 bulk=1、verify=0、載入 2,781,269-byte cache，停在標題畫面；
這是掌機沒電前的最後確認狀態，不表示目前裝置正在運行。

debug.env 已逐 byte 還原為：

```ini
SFR_SKIP_MOVIES=0
SFR_PARALLEL_WORKER=cores
```

測試用自動啟動、skip-movies workaround、額外追蹤和 A/B flags 均已移除。
一般 settings 保持 render_every=1、vsync=0、60 FPS cap、cores。
預設保留 completion handoff、Vulkan pipeline cache、bulk key；critical wait
仍是原 global 政策。`SFR_CRITICAL_WAIT_GLOBAL=0` 是未證明改善的實驗，不要默默啟用。

本機 evidence：

- `out/android-performance-round3/`：許可測量、baseline／candidate、measure.py。
- `out/android-startup-round4/`：漏喚醒 trace 與修正後冷啟動。
- `out/android-pipeline-round5/`：快取 A/B、APK、比較及 cache。
- `out/android-logging-round6/`：日誌 A/B、simpleperf、按 TID 的報表；symbols/libmain.so
  對應 build ID `f8c5b577dce2e847d1b9020c090833ecc640d308`，不要拿新 round7 符號覆蓋它。
- `out/android-pipeline-key-round7/`：最新 APK、build identity、回歸／microbenchmark、
  verify.log、四趟 comparison.json、原始 log／截圖、delivery.log/png、original-debug.env。

導航／capture 腳本在 round7；按鍵用 adb `keyevent --longpress`，短按可能被遊戲漏掉。
CPU profiling 需要另外打包 profileable APK；完成後換回正常包。不要用 verify=1
或正在取樣的格當正常 FPS。依序佔用單一掌機，避免另一位 agent 同時重啟／安裝／
改 debug.env，並保留暖快取，不要隨意清 app data。

## 接下來兩個 worktree 的安排

先把本工作區的共同成果提交成可辨識基準，再讓兩邊從該 commit 建立各自分支和
worktree；不要只從目前未包含這些修改的 HEAD 開始。使用者只是提出未來可能安排，
本次沒有代為建立 worktree。

每邊使用自己的 source checkout、build 目錄、generated output、jniLibs staging、
APK output、執行時 save/cache/log。不要共用可寫的 out/build，也不要直接複製
CMakeCache.txt：其中有舊工作區的絕對路徑。ignored 的 tools、generated code、
shader pack、private game inputs 必須另行 bootstrap／準備；新 worktree 不會自動
帶過去。可以明確共享唯讀輸入，但不要讓兩邊同時改同一個 Plume checkout／產物。

建議把 renderer／頂點宣告工作與執行緒／記憶體分析分工，避免同時改
`diagnostic_main.cpp`、`guest_graphics_hooks.cpp`、`native_renderer.cpp`；交叉修改先
約定擁有者。CMake 與 performance.md 仍可能有衝突，可先各寫自己的實驗紀錄再整合。
一個 agent 測掌機時，另一個做 host tests 或離線分析；worktree 不會隔離同一台裝置。

## 下一步可接的題目與不要誤判的事

1. Pipeline key 填入已完成；頂點宣告每 draw 重建仍未優化。若加快取，需要包含
   guest 內容變動的失效條件，不可只靠相同 declaration 位址。
2. round6 取樣：主客體 native_draw inclusive 約 26.96%，NativeRenderer::draw 14.01%
   是其子集合。guest 29 的 827C9508→827D7250→827D66C0→827D6088 呼叫鏈占該
   執行緒約 95.52%，語意尚未判定，可以離線分析。
3. CAS 的 659 個樣本中 603 個來自 GuestMemory 與 libc++ shared_mutex，491 個經過
   check_reservation_context，集合重疊；主執行緒只有 2 個 CAS。PCR／thread object
   只 commit 部分頁，導致檢查走 slow path，是已找到的具體原因；直接擴大 commit
   或移除鎖會改變保護／生命週期語意，尚未實作，也未證明在主執行緒關鍵等待路徑上。
4. 建立可重複的輸入／場景重播，會讓後續小幅改善比較可信。尚無證據能承諾 30 FPS，
   也不能僅由全程序最大 leaf 百分比小，就認定架構已無優化空間。

細部根因、測量表與各輪限制請以 `docs/performance.md` 的 2026-09-25 各節為準；
早期 round3「開場尚未解決」是歷史紀錄，後面 round4 已補上根因與修正。
