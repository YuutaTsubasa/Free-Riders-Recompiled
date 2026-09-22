# 實作與驗證紀錄

日期：2026-09-10。**整體五平台原生遊戲移植尚未完成。** 已完成來源工具、Windows CPU 診斷基礎、最小模組資訊／XEX 標頭查詢、初始虛擬記憶體保留／提交、使用者程序類型、臨界區初始化及硬體 flags 讀取，未到達遊戲標題畫面。

## 最新進度

**已進入主選單。** 以 NUI API 層級的 Kinect 模擬（手把控制手部游標）通過標題 START、系統提示與「Omochao 教學」詢問，原生畫出 Main Menu。詳見 [從標題畫面進入主選單](main-menu.md)。

**手把直接操作選單。** 按鍵對應遊戲原有的 Kinect 語音指令（START／A＝ok／B＝back／十字鍵＝方向），只認手的是非對話框改由 A／B 直接選擇；已只用按鍵從標題走到 Time Attack 的角色選擇。詳見 [用手把操作選單](pad-menus.md)。

**手把可以把教學關一段一段跑下去。** 分段選單靠 A（`ok`）選取對焦的環狀項目推進；
手把 X 另外會說映像詞彙表裡的 `next`，給其他有 Next 按鈕的頁面用。一般比賽仍卡在裝備零件畫面：換頁的延遲動作永遠不到期，因為遊戲
在那一頁不再呼叫選單更新，而那個動作本身是頁面的淡出。詳見 [用手把操作選單](pad-menus.md)。

**比賽畫面已經完整顯示。** 解析出來的複本原本用虛擬位址當鍵、繪製取樣用實體位址，所以
最後的合成取樣不到場景而畫成全黑；換算之後教學關的比賽有完整的 3D 場景與 HUD。詳見
[比賽畫面](race-scene-rendering.md)。

**比賽的 3D 場景能算繪出來。** 1182 個執行期著色器裡原本有 735 個
翻譯不出來（骨骼調色盤的頂點抓取不在頂點宣告裡、迴圈常數由繪製時設定），補在我們自己的
前處理／後處理之後只剩 3 個；比賽中解析時傾印框架緩衝可以看到完整場景。畫面上仍只有 2D，
因為所有算繪目標都別名到同一組框架緩衝。詳見 [骨骼調色盤與迴圈常數](shader-vertex-palette.md)。

**比賽中的操作已實作，但還沒跑完一場比賽。** 身體狀態改由手把填入、約 25 個手勢判斷器改看手把、賽前「On your Gear!」量測動作跳過；教學關可以載入並進入比賽場景，但比賽畫面需要離屏算繪目標，原生後端尚未支援。詳見 [用手把進行比賽](race-controls.md)。

原生畫面已從「Now Loading」一路顯示到 ESRB、SEGA、Sonic Team 標誌與標題影片（「SONIC FREE RIDERS」標誌與遊戲自己解碼的 WMV 影片，與參考幀一致）。詳見 [從載入畫面到標題影片](title-screen.md)。

紋理綁定已逐位元重現原始的 fetch constant 合併，著色器改由原始建立函式產生真正的客體物件並對應到原生著色器，頂點宣告與著色器設定函式可以直接執行原始程式碼。實跑已到達第一次 Draw（`824F5288`，三角形帶、4個頂點）。詳見 [紋理綁定與原始著色器物件](textures-and-shader-objects.md)。

`XamInputGetState` 已由 XInput 加鍵盤支援，`SetRenderState(BLENDOP)` 已加入原生混色狀態。實跑現在停在 `SetTexture`，也就是第一次綁定真正的紋理。詳見 [原生輸入與 BLENDOP](native-input.md)。

原遊戲第一次 Present（`824E65A0`：把 RT0 resolve 到前緩衝區後交換）已由原生呈現執行。之後遊戲在工作執行緒上呼叫 `XamInputGetState`，現在停在這裡。尚無原遊戲 Draw。詳見 [第一次 Present](first-present.md)。

繪圖改走「HLE + 先盤點」：遊戲直接呼叫208個 D3D 函式，已有原生處理的是36個。原生裝置新增可丟棄的命令緩衝區，經審核的「只寫封包」函式可以直接執行原始程式碼。實跑越過 `824EC0A8`（`SetShaderGPRAllocation`），現在停在 `824E65A0`（渲染目標保存/清除/還原）。詳見 [D3D 盤點與只寫封包函式](d3d-packet-writers.md)。

帶更新的載入/儲存（14種）與逐元素向量整數運算（24種）已由表驅動改寫器及有測試的輔助函式支援，改寫4,042處指令；停用函式由479降為143。實跑停止點與執行過的函式不變，本次實跑尚未執行到新指令。詳見 [補充指令](supplemental-instructions.md)。

新的跳轉表分析器取代 XenonAnalyse，辨識出309個 switch 表，其中294個以「依 CTR 目標分派、未知目標停止」的方式保留；停用函式再由628降為479，`error_comment` 由152降為2。實跑停止點與執行過的函式不變，本次實跑尚未走到新的跳轉表。詳見 [跳轉表](jump-tables.md)。

線性掃描改為略過填充字組及兩個C++例外處理常式字組，停用函式由14,475降為628；舊數字大多是錯位碎片，不是真正的轉譯缺口。實跑停止點不變。下一個缺口是未辨識的switch跳轉表。詳見 [函式邊界](function-boundaries.md)。

26個取樣器的原始預設值、MIN／MAG與原始 inline MIP 修改已通過真實啟動，三項過濾為LINEAR。目前停於 `824EC0A8`，LR `8249BC68`。65項CTest／192項Python通過，無略過；標題／主選單仍未達成。詳見 [最新啟動證據](sampler-filters.md)。

原始混色預設值已從11筆實際資料建立，啟用、來源因子及目的因子的四個目標更新已通過真實啟動。目前停於取樣器設定 `824E8248`。64項CTest／192項Python測試通過，無略過；尚無原始Draw／Present或標題至主選單。詳見 [最新啟動證據](blend-requests.md)。以下保留先前停止點。

四個未選定玩家欄位已經由原始流程完成資料清除及空姓名Unicode轉換，後續原始深度與剔除設定也已通過；目前停於 `824E6A40`。63項CTest／192項Python測試通過；原始標題／主選單仍未達成。詳見 [最新啟動證據](user-initialization.md)。

原始流程已建立真正事件支援的通知監聽器、保存handle並要求下方中央通知位置，接著停於 `XamUserGetSigninState`。60項CTest／192項Python測試通過；尚無原始標題或主選單。詳見 [最新啟動證據](native-notifications.md)。

原始遊戲已選用自身的plain Winsock回退路徑，成功呼叫真正Windows初始化，接著停於 `XamNotifyCreateListener`。57項CTest／192項Python測試通過；標題／主選單仍未達成。詳見 [最新啟動證據](native-winsock.md)。下列保留先前停止點的歷史紀錄。

原始較新版本分支已取得XAM模組識別，接著要求 `NetDll_WSAStartupEx`。56項CTest／192項Python測試通過；標題／主選單仍未達成。詳見 [最新啟動證據](xam-system-version.md)。

原始流程已完成兩次Kinect裝置缺席查詢，接著停於 `XamGetSystemVersion`。56項CTest與192項Python測試通過；標題／主選單仍未達成。詳見 [裝置狀態查詢證據](nui-device-status.md)。

原始解碼函式的六個向量部分讀取已接通，真實啟動已越過824E46A0停止點，新增啟動五個原始工作執行緒後停於 `XamNuiGetDeviceStatus`。55項CTest、192項Python測試通過。原解碼輸出內容及標題／主選單仍未驗證；詳見 [本次證據](partial-vector-loads.md)。以下保留各項依賴的歷史紀錄。

## 階段狀態

使用者已將當前驗收改為 [Windows 真實標題畫面經鍵盤或手把確認進入主選單](title-menu-goal.md)。**目前未達成**，入口已由原始效果解析取得全部68個原生 shader 資源，並通過裝置 AddRef及原始效果初始化，經受檢查的lhzu轉譯建立四份原始頂點宣告並逐一核對metadata與元素，接續完成原始裝置保存、配置及真正的Windows semaphore/event建立與event重設，再依原始要求建立兩個真正暫停中的Windows工作執行緒及獨立PCR／TLS／stack，完成物件引用、優先權與處理器設定，再恢復第二個執行緒並執行原始XAPI／工作函式，接通可取消的原生事件等待與64位元原子操作，完成原始渲染器建構及除錯監視器缺席分支，已按原始建立旗標建立第九個原生工作執行緒，綁定處理器並恢復後執行原始工作函式與事件等待，已通過執行中遊戲模組查詢，已取得原始TEX_00資料並核對雜湊，經原始包裝函式交至DDS解析器，已通過bdzf轉譯，由原始DDS handler解析出64×64 DXT5資料並進入原始複製流程，已接通目的與來源的保護屬性查詢，已通過dcbz執行原始複製並核對4096bytes位元組交換及來源不變，DDS解析器已完成並更新所有權；原始參數與配置流程已繼續，已通過受檢查的addc／addme轉譯並進入原始紋理傳輸與尺寸計算流程，已接通真正的原生佇列同步，原始貼圖傳輸已成功返回0並核對解鎖後header恢復；已通過共享表面釋放，已完成真正的整段虛擬記憶體釋放，原始貼圖建立已有21次成功返回；已執行原始渲染器初始化的CPU設定，核對64-byte矩陣複製、能力資料及1280×720參數，遊戲自行選定shader等級3；已通過原始混色狀態設定並保存原生管線描述；已通過原始七項alpha／depth／cull設定並保存原生狀態；已通過原始八個null貼圖綁定，經受檢查的stfsu轉譯完成1440-byte原始浮點資料表初始化，並通過十二份原始頂點宣告建立；已完成十二份原始宣告的內容核對及六個內嵌shader建立與保存，並通過原始primitive restart及eqv初始化；已接通 Windows 實際介面語言查詢，原始流程收到繁體中文值8後繼續；已以明確的北美執行設定通過 `XGetGameRegion`，核對原始語言選擇結果並繼續；已建立原始具名事件並啟動骨架資料工作執行緒，實際進入事件等待；已接通Windows國家查詢、保留兩段原始函式內跳轉並補上執行緒鎖等待，實跑原始國家轉換返回34；已完成FNT_SE真正非同步讀取，14774 bytes與原資產雜湊一致，原始執行緒已收到通知、辨識XCTD檔頭並進入解壓縮，目前停於824E46A0缺少的向量部分載入；仍需解碼完成及實際資源／shader／PSO消費，尚無遊戲畫面。為達成此成果，可跨 CPU/runtime、載入、繪圖和輸入處理必要依賴；不再要求各技術階段全部完成後才碰下一階段，也不提前展開其他平台或單場遊玩。下表保留為技術缺口清單，不能當成使用者成果已達成。先前 Linux／Android 核心驗證只屬提前準備。

| 階段 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| M0 來源與工具鏈 | 已完成本版 ROM 的工具與驗證 | 完整 685 檔案擷取及逐檔 SHA-256；固定 Xenon 工具成功建置；來源安全回歸通過 |
| M1 CPU 重編譯 | 部分完成 | 真正入口已通過模組標頭查詢；仍有指令、函式邊界與 runtime 缺口，尚未執行完整啟動 |
| M2 遊戲啟動 | 未完成 | 已通過 CRT spin count 初始化、TLS、系統時間與即時計時，及初始化原子迴圈，及向量靜態資料初始化，已取得原生 XAM 模組識別，原始程式已處理派對服務缺席，普通 subfze 已有受檢查的轉譯，已通過記憶體統計，已完成原生顯示查詢，已配置首筆0x7202000 bytes，write-combine配置也已生效，已接通原生D3D12裝置／清除／viewport、真實shader資產讀取與68個shader資源建立，已通過sthu轉譯完成原始效果初始化，已經lhzu轉譯完成四份原始頂點宣告，並通過原始裝置保存與原生semaphore/event建立、重設，已建立兩個真實暫停工作執行緒，完成物件引用與原生優先權／處理器設定，已執行原始XAPI與工作函式，接通可取消的原生事件等待，已通過64位元原子操作，已完成原始渲染器建構及監視器缺席分支，已處理執行緒建立處理器旗標，已接通遊戲模組識別查詢，已取得原始TEX_00資源並交至原始DDS解析器，已通過bdzf轉譯，由原始DDS handler解析出64×64 DXT5資料並進入原始複製流程，已接通目的與來源的保護屬性查詢，已通過dcbz執行原始複製並核對4096bytes位元組交換及來源不變，DDS解析器已完成並更新所有權；原始參數與配置流程已繼續，已通過受檢查的addc／addme轉譯並進入原始紋理傳輸與尺寸計算流程，已接通真正的原生佇列同步，原始貼圖傳輸已成功返回0並核對解鎖後header恢復；已通過共享表面釋放，已完成真正的整段虛擬記憶體釋放，原始貼圖建立已有21次成功返回；已執行原始渲染器初始化的CPU設定，核對64-byte矩陣複製、能力資料及1280×720參數，遊戲自行選定shader等級3；已通過原始混色狀態設定並保存原生管線描述；已通過原始七項alpha／depth／cull設定並保存原生狀態；已通過原始八個null貼圖綁定，經受檢查的stfsu轉譯完成1440-byte原始浮點資料表初始化，並通過十二份原始頂點宣告建立；已完成十二份原始宣告的內容核對及六個內嵌shader建立與保存，並通過原始primitive restart及eqv初始化；已接通 Windows 實際介面語言查詢，原始流程收到繁體中文值8後繼續；已以明確的北美執行設定通過 `XGetGameRegion`，核對原始語言選擇結果並繼續；已建立原始具名事件並啟動骨架資料工作執行緒，實際進入事件等待；已接通Windows國家查詢、保留兩段原始函式內跳轉並補上執行緒鎖等待，實跑原始國家轉換返回34；已完成FNT_SE真正非同步讀取，14774 bytes與原資產雜湊一致，原始執行緒已收到通知、辨識XCTD檔頭並進入解壓縮，目前停於824E46A0缺少的向量部分載入；仍需解碼完成及實際資源／shader／PSO消費；仍需其他CPU/runtime、繪圖、特殊化及狀態綁定與音訊 |
| M3 單場遊玩 | 未完成 | 需 Kinect 操作替代、關卡載入、遊戲循環與存檔驗證 |
| M4 Linux/macOS | Linux 進行中 | 完整遊戲在 WSL2 Ubuntu 22.04（Vulkan＋SDL2）建置並執行，見 [Linux 版](linux.md)；macOS 未驗證 |
| M5 Android/iOS | Android 進行中 | APK（arm64-v8a、x86_64）在 Android 16 模擬器跑到標題畫面，見 [Android 版](android.md)；有觸控按鈕與傾斜轉彎；尚未實機驗證；iOS 未開始 |
| M6 發布品質 | 未開始 | 不提供可玩版本、完成時間或效能承諾 |

## 最新執行緒進展

64位元原子操作已接通，原始資料結構初始化已實際完成條件儲存；遊戲已建立八個真正 Windows 工作執行緒，恢復其中四個。原始渲染器建構已完成，三份單位矩陣已核對，並通過 Xbox 除錯監視器缺席分支；已按原始建立旗標建立第九個原生工作執行緒，綁定處理器並恢復後執行原始工作函式與事件等待，已通過執行中遊戲模組查詢，已取得原始TEX_00資料並核對雜湊，經原始包裝函式交至DDS解析器，已通過bdzf轉譯，由原始DDS handler解析出64×64 DXT5資料並進入原始複製流程，已接通目的與來源的保護屬性查詢，已通過dcbz執行原始複製並核對4096bytes位元組交換及來源不變，DDS解析器已完成並更新所有權；原始參數與配置流程已繼續，已通過受檢查的addc／addme轉譯並進入原始紋理傳輸與尺寸計算流程，已接通真正的原生佇列同步，原始貼圖傳輸已成功返回0並核對解鎖後header恢復；已通過共享表面釋放，已完成真正的整段虛擬記憶體釋放，原始貼圖建立已有21次成功返回；已執行原始渲染器初始化的CPU設定，核對64-byte矩陣複製、能力資料及1280×720參數，遊戲自行選定shader等級3；已通過原始混色狀態設定並保存原生管線描述；已通過原始七項alpha／depth／cull設定並保存原生狀態；已通過原始八個null貼圖綁定，經受檢查的stfsu轉譯完成1440-byte原始浮點資料表初始化，並通過十二份原始頂點宣告建立；已完成十二份原始宣告的內容核對及六個內嵌shader建立與保存，並通過原始primitive restart及eqv初始化；已接通 Windows 實際介面語言查詢，原始流程收到繁體中文值8後繼續；已以明確的北美執行設定通過 `XGetGameRegion`，核對原始語言選擇結果並繼續；已建立原始具名事件並啟動骨架資料工作執行緒，實際進入事件等待；已接通Windows國家查詢、保留兩段原始函式內跳轉並補上執行緒鎖等待，實跑原始國家轉換返回34；已完成FNT_SE真正非同步讀取，14774 bytes與原資產雜湊一致，原始執行緒已收到通知、辨識XCTD檔頭並進入解壓縮，目前停於824E46A0缺少的向量部分載入；仍需解碼完成及實際資源／shader／PSO消費。55項原生測試與188項Python測試通過，無略過；最新來源為 `out/recomp/diagnostic-format`，完整紀錄、命令及限制見 [最新原始非同步讀取證據](native-async-read.md)。標題／主選單驗收仍未達成。

## 實測結果

- ISO：7,838,695,424 bytes，SHA-256 `f15775c9a0eb1a09920794a169bb904ce5b0e38e05a06bc92b7c59707d964321`。
- XDVDFS：遊戲分割區 `0x0FD90000`，685 個檔案、4 個目錄。
- 完整資產在 `private/assets`，manifest 的 `complete=true`、`selection=full_disc`；擷取來源保持唯讀。
- XEX：14,241,792 bytes，Title ID `0x5345084D`，SHA-256 `3d58636bec9b92948f7dbe423c0aa96753349ba014194c4e62d2f809c14643a7`。
- 解碼映像：34,209,792 bytes，SHA-256 `d092c5397d769594d7b46cda023cf2c62aed9df4e76db44efee8136daf00ad99`。
- 299 個具名函式 imports，13 個 kernel 變數 imports。
- 產生 46,503 個函式定義和 46,802 個符號映射；分析包含重疊入口，不能當成已確認的真實函式數量。
- 原始轉換有 10,193 筆不支援指令事件及 87 筆條件暫存器比較遺漏事件，包含重複／重疊函式中的記錄。產生器的成功退出碼不足以通過語意驗收。
- 最新診斷保留 31,941 個函式的產生碼，14,562 個函式改為具名停止。已知缺口、原始輸入 hash 和逐函式原因保存在 `out/recomp/diagnostic-format/report.json`。
- XenonAnalyse 輸出零筆 switch entries。這是目前分析器未辨識出表格，不是遊戲沒有 switch。
- Shader 檔案：`shader/Xbox360BasicShader.fxobj`，93,676 bytes。音訊包含 `sound/SRN_BGM.cpk`、`*.csb`；只確認檔案存在，尚未解包或轉換 shader／音訊。

## 真實啟動追蹤

以下保留早期啟動片段；最新執行結果與完整命令見 [最新原始非同步讀取證據](native-async-read.md)：

```text
BIND XboxHardwareInfo address=0x71200000 flags=0x20
Free Riders CPU diagnostic: entry=0x824D22F0 mappings=46802 import_variables=13
ENTER _xstart @0x824d22f0
ENTER __savegprlr_28 @0x82a56068
ENTER sub_824DD048 @0x824dd048
ENTER sub_824DCF70 @0x824dcf70
IMPORT RtlImageXexHeaderField key=0x20401 result=0x0
ENTER sub_824E1018 @0x824e1018
ENTER __savegprlr_20 @0x82a56048
ENTER sub_82A54330 @0x82a54330
ENTER sub_82A561E0 @0x82a561e0
REQUEST NtAllocateVirtualMemory base_ptr=0x7013fb40 size_ptr=0x7013fc34 base=0x0 size=0x100000 type=0x60002000 protect=0x4 debug=0x0
RESULT NtAllocateVirtualMemory status=0x0 base=0x40000000 size=0x100000
REQUEST NtAllocateVirtualMemory base_ptr=0x7013fb44 size_ptr=0x7013fc3c base=0x40000000 size=0x10000 type=0x60001000 protect=0x4 debug=0x0
RESULT NtAllocateVirtualMemory status=0x0 base=0x40000000 size=0x10000
IMPORT KeGetCurrentProcessType result=0x1
IMPORT RtlInitializeCriticalSection address=0x40000618
ENTER sub_824E0908 @0x824e0908
ENTER __savegprlr_22 @0x82a56050
ENTER sub_824DF880 @0x824df880
ENTER __savegprlr_28 @0x82a56068
ENTER sub_824DF6A8 @0x824df6a8
ENTER __restgprlr_28 @0x82a560b8
ENTER sub_824E0090 @0x824e0090
ENTER __savegprlr_28 @0x82a56068
ENTER __restgprlr_28 @0x82a560b8
ENTER __restgprlr_22 @0x82a560a0
ENTER __restgprlr_20 @0x82a56098
ENTER sub_824DF608 @0x824df608
ENTER sub_824DCE98 @0x824dce98
ENTER __savegprlr_28 @0x82a56068
IMPORT RtlEnterCriticalSection address=0x82ad08f0 owner=0x70000bb0 recursion=1 lock_count=0x0
IMPORT RtlLeaveCriticalSection address=0x82ad08f0 owner=0x0 recursion=0 lock_count=0xffffffff
ENTER __restgprlr_28 @0x82a560b8
ENTER sub_824D2108 @0x824d2108
IMPORT XexCheckExecutablePrivilege privilege=10 result=0
ENTER sub_82A56FD8 @0x82a56fd8
ENTER sub_82A56A40 @0x82a56a40
ENTER sub_82A5F2B0 @0x82a5f2b0
ENTER sub_82A5B218 @0x82a5b218
ENTER sub_82A5F2A0 @0x82a5f2a0
ENTER sub_82A5F5E0 @0x82a5f5e0
ENTER sub_82A62C68 @0x82a62c68
ENTER __savegprlr_28 @0x82a56068
ENTER sub_82A62BD0 @0x82a62bd0
IMPORT RtlInitializeCriticalSectionAndSpinCount address=0x82c31af8 spin=4000 encoded=16 status=0
...（共 14 次初始化，位址每次增加 28 bytes）
ENTER __restgprlr_28 @0x82a560b8
IMPORT KeTlsAlloc result=0x0
...
IMPORT KeTlsSetValue index=0x0 value=0x400006a0 result=0x1
...
IMPORT KeQuerySystemTime output=0x7013fc70 ticks=<本次主機時間>
TIME_BASE ticks=<本次單調時基> frequency=49875000
STORE_CONDITIONAL address=0x82afee7c value=0x0 success=1
...（四個臨界區初始化與後續靜態初始化）
VECTOR_LOAD address=0x820bb520
VECTOR_STORE address=0x82b60ab0
...
IMPORT MmQueryStatistics output=0x70131510 status=0x0 budget_pages=131072 kernel_pages=12 available_pages=122627 virtual_capacity=536870912 reserved_virtual=1048576 stack_pages=64 image_pages=8352 virtual_pages=17 physical_pages=0
IMPORT XGetVideoMode output=0x70131530 source=windows-current-display width=1920 height=1080 refresh_hz=60 interlaced=0 widescreen=1 high_definition=1 guest_profile=reference-compatible
...
RESULT MmAllocatePhysicalMemoryEx base=0xf8afd000 committed_bytes=0x7203000
...
REQUEST MmAllocatePhysicalMemoryEx flags=0x0 size=0x12c0 protect=0x404 min=0x0 max=0xffffffff alignment=0x20
RESULT MmAllocatePhysicalMemoryEx base=0xf8afb000 committed_bytes=0x7205000 cache=write-combined
...
STOP import-function @0x82acbc2c: __imp__VdInitializeEngines
LAST_FUNCTION sub_825011A0 @0x825011a0 LR=0x825011dc calls=1271
```

退出碼 **3**。14 個 CRT 臨界區完成 spin count 初始化後，遊戲配置 TLS slot0，將 `0x400006A0` 寫入，再執行後續 CRT 配置；初始化計數器在 `0x82AFEE7C` 先由FFFFFFFF更新為0，觸發四個臨界區初始化，之後再次更新為1。本次write-combine配置使函式進入次數從1,267增至1,271；先前派對服務失敗分支使其從524增至1,030。目前已執行受檢查的 lvx128/stvx128，完成後續效能頻率查詢與三個臨界區初始化，Etx 追蹤服務明確回傳非成功的 C0000002，原本呼叫者繼續初始化，接著取得原生 XAM 模組識別，六項派對／社群匯出明確缺席，原始程式完成錯誤處理與 unload 後繼續；普通 subfze 轉譯解除函式入口限制後，已通過 [記憶體統計](memory-statistics.md)，兩次 [原生顯示查詢](video-mode.md) 完成後，首筆 [實體配置](physical-memory.md) 完成後，[write-combine配置](write-combine.md) 也已生效，新停止點為VdInitializeEngines；派對服務說明見 [派對匯出策略](party-export-policy.md)。模組契約見 [原生模組](native-modules.md)。此策略不代表 Etx 已實作，詳見 [Etx 調查](etx-investigation.md)。TLS 依 XEX 建立156 bytes 靜態資料及64個 slots，總共412 bytes；PCR 與 thread 的 TLS 指標已指向獨立區域。`LAST_FUNCTION` 是最近進入的函式，不是完整呼叫堆疊；超過前100次的 ENTER 不逐筆列印。實作契約見 [向量記憶體](vector-memory.md)、[保留／條件寫入](reservations.md)、[即時計時](timestamp-clock.md)、[系統時間](system-time.md)、[TLS](thread-local-storage.md)、[XEX loader／權限](xex-loader.md)、[臨界區操作](critical-section-locking.md)、[硬體資訊](hardware-info.md)、[程序／初始化](process-heap-init.md) 與 [虛擬記憶體](virtual-memory.md)。

第一階段 commit `f85e510` 的歷史結果如下；舊 `out/recomp/image` 不含新增標頭檔，不能直接用於新版診斷程式：

```text
Free Riders CPU diagnostic: entry=0x824D22F0 mappings=46802 import_variables=13
ENTER _xstart @0x824d22f0
ENTER __savegprlr_28 @0x82a56068
ENTER sub_824DD048 @0x824dd048
ENTER sub_824DCF70 @0x824dcf70
STOP import-variable @0x82000778: __imp__XexExecutableModuleHandle
LAST_FUNCTION sub_824DCF70 @0x824dcf70 LR=0x824dd058 calls=4
```

各階段追蹤均未證明完整遊戲邏輯正確，也不是成功啟動遊戲。

## 驗證與審查

- `scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-subfze`：成功建立完整診斷目標與工具；不需要修改上游原始碼。
- 完整 Python suite：**103 項通過，0 失敗，0 略過**，已設定全部原生測試環境變數，包括原始 XEX 標頭匯出測試。
- CTest：記憶體、XEX、配置器、臨界區、硬體、TLS、系統時間、單調時鐘、時間戳、向量記憶體、明確的不可用匯入策略、原生模組、整數運算、記憶體統計、顯示模式與實體配置，共 **16/16 通過**。
- 原生整合測試：正確映像重現上述追蹤；同大小錯誤映像、遭修改的匯入表在 guest 執行前拒絕。
- 固定依賴檢查通過：XenonRecomp `ddd128bcca99fe8bfbb99bea583c972351fa6ace` 與六個 submodule revisions 一致，無上游追蹤檔案修改。
- 獨立規格與品質審查發現並修正：XEX metadata 配額、重疊目錄範圍與累積讀取配額、診斷解析器的指令註解格式變更處理。各自新增回歸測試並重新審查。
- 建置另捕捉並修正 imports 的 C/C++ linkage 不一致；來源位址解析依 upstream 相對路徑規則修正。

詳細本機紀錄：`out/tests-final.log`、`out/diagnostic-build.log`、`out/boot-trace-final.log`、`out/recomp/status.json`。這些建置資料不納入 Git。

第一階段 `out/tests-final.log` 曾記錄 Windows 暫存目錄改名 `WinError 5`，後續完整重跑 `out/tests-final-rerun.log` 有 76 項通過；該環境錯誤根因尚未確認。Loader 階段紀錄為 `out/loader-red.log`（新增行為的預期失敗）、`out/loader-tests-final.log`（78 項通過）、`out/loader-final-build.log`、`out/loader-boot-final.log`。該階段映像為 `out/recomp/image-loader`，診斷來源為 `out/recomp/diagnostic-loader-final`；產生函式與停止函式數量維持不變，當時 299 個函式 imports 中僅標頭查詢有實作。Loader 的獨立規格與品質審查均通過。

虛擬記憶體階段已將診斷來源更新為 `out/recomp/diagnostic-vm`，沿用 `out/recomp/image-loader`。當時 299 個函式 imports 中，標頭查詢與記憶體配置這兩個有上述範圍內的實作；其餘仍明確停止。程式轉譯內容與函式計數沒有改變。記錄為 `out/vm-before.log`、`out/vm-allocate-red.log`、`out/vm-final-build.log`、`out/vm-tests-final.log`（82 項通過）、`out/vm-boot-final.log`。原生 CTest 三項通過，獨立規格與品質審查均通過。

本階段兩次完整測試再次出現 Windows 改名錯誤（`out/vm-tests.log`、`out/vm-tests-rerun.log`）。位置對照與移入 `out/test-tmp` 不能完全消除失敗。受控探針在 1,200 次測試中記錄 20 次 WinError 5，全部在 50 毫秒後成功，來源存在且目標尚未出現。產生器因此加入僅限 Windows 錯誤 5/32/33、最多五次的重試；目標出現或永久失敗仍報錯，沒有取消原子發布或覆蓋保護。四項故障注入回歸先失敗後通過，修正後 800 次診斷生成測試全部通過。相關紀錄為 `out/vm-rename-retry-probe.log`、`out/vm-publication-red.log`、`out/vm-publication-green.log`、`out/vm-publication-repeat.log`；此修正也經獨立複查。造成短暫拒絕的程序尚未識別。

程序／臨界區階段沿用同一份診斷產生碼，299 個函式 imports 有四個在記錄範圍內實作：XEX 標頭查詢、虛擬記憶體配置、使用者程序類型及臨界區初始化。記錄為 `out/process-red.log`、`out/critical-red.log`（新增整合斷言的預期失敗）、`out/process-final-build.log`、`out/process-tests-final.log`（82 項通過、無略過）、`out/process-ctest-final.log`（4/4）、`out/process-critical-boot.log`（退出碼 3）。獨立規格與品質審查均通過。鎖定、解鎖及執行緒排程沒有成功 stub。

本次硬體資訊階段新增一個變數匯入綁定：13 個變數中，模組 handle 與硬體 flags 已有最小資料，其餘 11 個仍受保護；函式 imports 實作數維持四個。硬體資料只映射 flags，未提供的後續欄位仍不可存取。記錄為 `out/hardware-boot-red.log`、`out/hardware-build-final.log`、`out/hardware-tests-final.log`（82 項通過、無略過）、`out/hardware-ctest-final.log`（5/5）、`out/hardware-boot-final.log`（退出碼 3）。規格審查指出的 byte+4 寫入測試已補齊並通過；規格與最終品質審查均批准。

## 下一個技術工作包

臨界區操作階段新增 Enter/Leave，支援範圍內的函式 imports 增至六個。記錄為 `out/lock-boot-red.log`、`out/lock-build.log`、`out/lock-tests-final.log`（82 項、無略過）、`out/lock-ctest-final.log`（5/5）、`out/lock-boot.log`。規格與品質審查均通過；直接注入錯誤 PCR／匯入配對的 dispatch 負向測試尚未加入，已有狀態函式負向測試與真實入口正向測試。競爭與排程仍不支援。

權限查詢階段以快取的原始系統 flags 回答位元0–31；範圍外索引先停止，不執行未定義移位。函式 imports 支援數增至七個。記錄為 `out/privilege-boot-red.log`、`out/privilege-build-final.log`、`out/privilege-tests-final.log`（82 項、無略過）、`out/privilege-ctest-final.log`（5/5）、`out/privilege-boot.log`。審查指出的相鄰 kind1 測試辨別力不足已補強；規格與品質審查均通過。

Spin count 階段依固定 Xenia 來源完成向上取整、255 上限與 status0；加法會溢位的輸入範圍明確停止。函式 imports 支援數增至八個。記錄為 `out/spin-boot-red.log`、`out/spin-build.log`、`out/spin-tests-final.log`（82 項、無略過）、`out/spin-ctest-final.log`（5/5）、`out/spin-boot.log`。規格審查指出的 guarded tail 測試已補齊；規格與最終品質審查均通過。

TLS 階段完成單一已知執行緒的靜態資料與 slots，以及 Alloc/Free/Get/Set 四個匯入，支援範圍內的函式 imports 增至十二個。真實路徑目前只觀察到 Alloc/Set；其餘由原生測試驗證。記錄為 `out/tls-boot-red.log`、`out/tls-build.log`、`out/tls-tests-final.log`（82 項、無略過）、`out/tls-ctest-final.log`（6/6）、`out/tls-boot.log`。規格與品質審查均通過。TLS slots 耗盡、無效索引和其他執行緒情境仍明確停止。

系統時間階段完成 void 匯入、主機時間轉換及完整8-byte檢查，支援函式 imports 增至十三個。記錄為 `out/time-boot-red.log`、`out/time-build.log`、`out/time-tests-final.log`（82項、無略過）、`out/time-ctest-final.log`（7/7）、`out/time-boot.log` 及 `out/time-boot-repeat.log`。兩次時間值不同但停止點一致；規格與品質審查均通過。

即時計時階段新增第三個變數綁定（其餘10個仍受保護）及同頻率查詢，支援範圍內函式 imports 增至十四個。新產生碼為 `out/recomp/diagnostic-clock`，20處 mftb 轉成單調時基 hook；函式保留／拒絕數不變。記錄為 `out/clock-generator-red.log`、`out/clock-generator-green.log`、`out/clock-generate.log`、`out/clock-boot-red.log`、`out/clock-build.log`、`out/clock-tests-final.log`（84項、無略過）、`out/clock-ctest-final.log`（9/9）、`out/clock-boot.log`。readonly 多欄位失敗的 RED/GREEN 紀錄為 `out/readonly-critical-*.log` 和 `out/readonly-vm-*.log`。規格審查通過；品質審查指出的 LP64 initializer-list 型別衝突已修正，最終複查通過。Clock/bundle 測試時序偏差詳見契約文件。

保留／條件寫入階段以受檢查 hooks 取代已核對的 lwarx/stwcx. 發射碼，新增保留378個函式；記憶體存取與服務干擾仍停止。新目錄 `out/recomp/diagnostic-reservations` 記錄468處保留讀取、498處條件寫入及27處時基讀取。記錄為 `out/reservation-generator-red.log`、`out/reservation-generator-green.log`、`out/reservation-generate.log`、`out/reservation-boot-red.log`、`out/reservation-build.log`、`out/reservation-tests-final.log`（86項、無略過）、`out/reservation-ctest-final.log`（9/9）、`out/reservation-boot.log`。規格與品質審查均通過；PPCContext 錯誤路徑尚無專用單元測試，已檢查程式順序。

向量記憶體階段加入完整 lvx/lvx128/stvx/stvx128 的位元序、完整範圍及寫入權限檢查。新增保留1,136個函式；真實入口通過靜態向量初始化並到達347次函式進入。記錄為 `out/vector-generator-red.log`、`out/vector-generator-green.log`、`out/vector-generation.log`、`out/vector-boot-red.log`、`out/vector-build-final.log`、`out/vector-tests-final.log`（91項、無略過）、`out/vector-ctest-final.log`（10/10）及 `out/vector-boot.log`。規格與品質審查均通過。

已新增未知匯入的原始暫存器追蹤；真實 Etx 呼叫者由 LR 確認為 `sub_8290B170`，停止點與347次計數不變。兩處呼叫端、暫存器實值及未確認的 ABI 記錄於 [Etx 調查](etx-investigation.md)。未實作推測性服務回傳。紀錄為 `out/import-context-red.log`、`out/import-context-build.log`、`out/import-context-boot.log`、`out/import-context-tests-final.log`（91項、無略過）與 `out/import-context-ctest-final.log`（10/10）；兩項獨立審查通過。

向量 word store 階段新增 stvewx/stvewx128 的四位元組受檢查寫入，原始607處中有133處位於本版保留函式，其他仍因各自的缺口停止。新增保留10個函式，沒有新增拒絕函式。最新目錄 `out/recomp/diagnostic-vector-words` 記錄7,224處完整向量讀取、7,338處完整寫入與133處 word store；啟動仍停在 Etx、347次函式進入。記錄為 `out/vector-word-generator-red.log`、`out/vector-word-generator-green.log`、`out/vector-word-generation.log`、`out/vector-word-build.log`、`out/vector-word-tests-final.log`（94項、無略過）、`out/vector-word-ctest-final.log`（10/10）與 `out/vector-word-boot.log`。規格與品質審查均通過。

Linux 核心驗證：以既有 Arch Linux／WSL2 的 GCC 14.2.1 編譯並執行同一批10個原生測試，全部通過，無需修改來源。已走過 POSIX mmap/mprotect 與4096-byte主機頁面；完整產生碼診斷仍未在 Linux 建置。重現方式見 [Linux 核心驗證](linux-core.md)，紀錄為 `out/linux-core-verification.log`。

Android 核心建置驗證：以本機 NDK r29-beta2／Clang20 建立10個 ARM64 API26 測試 ELF，所有 LOAD 區段皆為16 KiB對齊。未連接裝置，沒有 Android 執行結果，也尚無 APK。原始碼不需修改；16 KiB runtime 主機頁面相容性仍待處理。重現與限制見 [Android 核心建置](android-core.md)，紀錄為 `out/android-core-build.log` 與 `out/android-core-elf-check.log`。

1. 依 Etx 調查補足 producer context 的輸出配置、狀態及失敗契約，再實作追蹤服務；其餘向量記憶體形式繼續保護。
2. 依初始化路徑逐項提供其他記憶體配置、TLS／執行緒、同步和檔案服務；每次保留新的停止點及回歸測試。
3. 確認 `.pdata`／無 stack 函式的邊界、資料和程式混合區，以及本遊戲的 switch pattern。補齊所需 PowerPC/VMX 指令並與獨立語意測試比對；不得把診斷停止改成成功 stub。
4. 在 CPU 啟動可信後分析 FXOBJ、圖形狀態、CPK／CSB 和 NUI 遊戲動作接點，再進行桌面及手機平台的真正移植。

Etx 不可用策略保留14個已實作的 bounded imports，另列3個明確回傳非成功狀態的追蹤服務。未寫入 producer context，未修改遊戲分支；實際呼叫者自行繼續，入口由347推進至523次，停於載入 xam.xex 的 XexLoadImage。紀錄為 `out/etx-unavailable-build.log`、`out/etx-unavailable-boot.log`、`out/etx-unavailable-tests-final.log`（94項、無略過）與 `out/etx-unavailable-ctest-final.log`（11/11）。固定依賴檢查、獨立規格與品質審查均通過。尚未達到標題／主選單目標。

原生 XAM 模組紀錄提供有邊界檢查的穩定識別、載入計數與輸出寫入，未提供猜測的 LDR 結構或派對服務。實際入口由523推進至524次，原始呼叫者取得 handle 後查詢 ordinal0xAFF，停於 XexGetProcedureAddress。實作範圍為15個 bounded imports，Etx三個不可用策略另列。紀錄為 `out/native-modules-build.log`、`out/native-modules-boot.log`、`out/native-modules-tests-final.log`（94項、無略過）與 `out/native-modules-ctest-final.log`（12/12）。固定依賴及獨立規格／品質審查均通過；元件初始 RED 為缺少來源的編譯錯誤，之後另以可編譯 no-op 變異驗證測試會拒絕缺少 handle 寫入，並非回溯式 TDD。下一項必要缺口為匯出解析與該真實呼叫路徑；尚無標題／選單畫面。

派對／社群匯出缺席策略明確排除六項原生能力，由原始遊戲逐項處理 C0000263、轉為127並清除綁定及 unload；沒有提供可呼叫的假函式或修改遊戲分支。完整入口由524推進到1,030次，目前在 sub_82211798 入口因82211D68的 subfze 未轉譯而停止。支援範圍18個 bounded import handlers，Etx三個不可用策略另列，六項缺席匯出不算已實作服務。紀錄為 `out/party-exports-build-final.log`、`out/party-exports-boot.log`、`out/party-exports-tests-final.log`（94項、無略過）、`out/party-exports-ctest-final.log`（12/12）。固定依賴與獨立規格／品質審查通過；測試曾因先啟動 reservation 才做準備配置失敗，已修正測試順序並重建原生目標。下一步需依來源建立 subfze 語意及受檢查的轉譯；不得僅移除不支援日誌。

普通 subfze 階段新增 unsigned64 結果／低32 carry 的受檢查運算，只解除同一函式內指令位置、普通格式、空白輸出及全部日誌事件精確相符的停止原因。新增保留40個函式，沒有新增拒絕函式；最新 `out/recomp/diagnostic-subfze` 保留31,114函式、拒絕15,389，含72處普通 subfze。入口由1,030推進至1,037，停於 MmQueryStatistics；尚未走到新函式較後面的 subfze。規格審查發現畸形名稱無日誌時可能漏判，新增回歸並修正後通過規格／品質複查。修正後重新生成至 `out/recomp/diagnostic-subfze-verified`，187個程式／支援檔與已建置來源逐檔相同，report僅generator_sha256改變；比較證據 `out/subfze-generation-comparison.log`。其他記錄為 `out/subfze-helper-red.log`、`out/subfze-helper-green.log`、`out/subfze-generator-red.log`、`out/subfze-generator-p2-red.log`、`out/subfze-generator-green.log`、`out/subfze-build.log`、`out/subfze-boot.log`、`out/subfze-tests-final.log`（103項、無略過）、`out/subfze-ctest-final.log`（13/13）。固定依賴通過。下一步需建立與實際配置一致的104-byte記憶體統計。

原生記憶體統計階段加入可強制執行的512MiB backing 預算，依實際保留／提交範圍編碼104-byte統計。保留不消耗 backing、重複提交不重複計算、無效輸出先完整檢查再寫入。19個 bounded import handlers；真實入口成功查詢後由1,037推進至1,039次，停於 XGetVideoMode（82ACB23C）。證據見 [記憶體統計](memory-statistics.md)：103項Python、14/14原生測試、固定依賴及獨立規格／品質審查通過。下一步建立顯示查詢與原生顯示設定，仍無標題畫面或確認輸入。

原生顯示模式階段將 XGetVideoMode 接至 Windows 當前顯示查詢；本機回傳1920×1080、60Hz逐行。保留48-byte原始ABI並完整檢查寫入，void匯入不修改暫存器。真實遊戲完成兩次查詢後從1,039推進至1,063次，停於 MmAllocatePhysicalMemoryEx（82ACBA0C），要求0x7203000 bytes。現在20個 bounded import handlers；103項Python、15/15原生測試、固定依賴和獨立規格／品質審查通過。完整證據與協定限制見 [顯示模式](video-mode.md)。下一步建立這項配置的實際 backing 與位址語意，尚無遊戲畫面或輸入驗收。

基本實體配置階段以原生backing建立4KiB實體位址view，原始遊戲取得0xF8AFD000、大小0x07203000，初始化從1,063推進至1,267次。下一筆0x12C0 bytes要求0x404保護／快取屬性，目前明確停止。MemoryStatistics新增title physical分類並排除kernel重複計數；21個bounded import handlers。103項Python、16/16原生測試、固定依賴及獨立規格／品質審查通過。測試修正一次耗盡預算遮蔽occupied-range分支的假陽性，已重建驗證。詳見 [實體配置](physical-memory.md)。下一步處理write-combine屬性，仍未達成標題至主選單。

Write-combine 階段以 Windows VirtualAlloc2 placeholder 分割與獨立 backing 保留真實快取屬性；VirtualQuery 確認普通頁為4、write-combine頁為404。受檢查寫入包含 MFENCE，保留字原子操作仍明確拒絕此類頁面。原始遊戲第二筆配置取得0xF8AFB000，實體 backing 合計0x07205000 bytes；入口從1,267推進至1,271次，停於 VdInitializeEngines（82ACBC2C）。完整回歸抓到已隔離 placeholder 重複分割問題，修正後16/16原生測試、103項Python重跑、固定依賴及規格／品質審查通過。前次Python因既有暫時檔案鎖定測試的重試次數偶發失敗，失敗紀錄與限制保留於 [write-combine證據](write-combine.md)。尚無圖形引擎、標題畫面或確認輸入；接下來需建立真正繪圖路徑。

原生圖形裝置階段固定並連結 Unleashed 所用的 Plume，從原始 D3D 建立接點824F4CF0初始化實際 RTX4090 D3D12裝置與direct queue，保留原始guest流程及1271次／VdInitializeEngines停止點。兩次4096-byte不同資料的GPU複製／讀回通過，完整17/17原生、103項Python及依賴驗證通過，規格／品質審查通過。補查原生queue handle以避免只驗證上游wrapper非空；另記錄上游internal timestamp queue失敗路徑限制。已核對Free Riders專用裝置／shader／draw／Present接點；68個原始basic shader容器全部轉譯與DXIL library編譯，另以明示測試specialization值連結一組原始VS/PS。原始遊戲尚未提交draw或呈現畫面，仍需完整D3D適配與遊戲確認輸入。證據與後续工作見 [原生圖形裝置](native-graphics.md)。

原生裝置／清除適配階段以 strong public hooks 接通原遊戲824F4CF0及824F6DC8，建立真正1280×720色彩、深度／stencil、交換鏈與隱藏視窗；原始入口成功提交 flags3F 的 GPU 清除，停於下一個視口設定824E96C8。函式進入數為1,251，因替換Xbox驅動內部初始化而少於先前1,271。真實視口請求確認反向深度1→0。GPU測試找出Windows小視窗最小尺寸及固定Plume的buffer-copy空指標，修正後完整19/19原生、103項Python（無略過）、4項更新後啟動檢查及依賴驗證通過；兩個元件各自完成規格／品質審查。另修正native初始化檢查順序、無效swapchain wrapper銷毀及DXGI waitable handle清理。尚未呼叫原遊戲draw／Present或處理確認輸入，標題／主選單目標仍未達成；下一項必要依賴為保留真實反向深度的視口／scissor狀態。詳細證據、重現命令與上游失敗路徑限制見 [原生呈現適配](native-presentation.md)。

原生視口／ANSI 檔名描述階段接通824E96C8、scissor及enable三個原始接點，查詢實際 D3D12 OPTIONS13 並保留遊戲1280×720、深度1→0。獨立 GPU 測試實際繪製並讀回，確認 viewport/scissor 覆蓋及0.25→0.75深度映射；原始欄位、dirty bits、溢位拒絕及清除裁切有回歸驗證。原遊戲随后建立37-byte基本 shader 檔名描述，入口到1,265次，停於 NtCreateFile（82ACB58C），尚未實際讀檔。完整21/21原生、103項Python（無略過）、固定依賴與各元件規格／品質審查通過。GPU測試修正footprint對齊／總長計算，規格審查抓到signed端點溢位並以RED/GREEN驗證修正；沒有改動固定vendor。22個bounded import handlers；仍無原遊戲draw／Present、標題／主選單或確認輸入。下一項為讀取原始shader資產所需的受檢查檔案介面。詳見 [原生視口與檔名描述](native-viewport.md)。

原生唯讀檔案階段將原始 NtCreateFile 接到已掛載資產目錄，保留實際 Windows handle、共享模式及檔案大小；原遊戲取得72000004、93,676 bytes的基本shader檔案並繼續到1,270次，停於 NtQueryInformationFile（82ACB66C、class22、56-byte輸出）。輸出BE結構、參數模式、完整範圍及重疊檢查先於host開啟；以opened-handle最終路徑檢查root containment，實測junction內部／逸出及exclusive sharing。23/23原生與104項Python（無略過）、固定依賴通過；資產SHA256與前次一致。CLI新增明示private/assets參數，未掛載時明確停止。尚未由遊戲讀入shader內容，也未draw／Present、標題／主選單或確認輸入。詳見 [原生資產開啟](asset-files.md)。

原生檔案資訊階段將 NtQueryInformationFile class34 接至 retained HANDLE 的實際基本／標準資訊，保留四個獨立時間、allocation、EOF與屬性，完整編碼56-byte BE結果；短長度／無效handle回傳NT錯誤且不改輸出，未知class／超長buffer／範圍及重疊明確停止。原遊戲取得EOF93676、allocation94208，自行配置緩衝區後於1,293次進入停在 NtReadFile（82ACB7BC），要求讀取93676bytes至40001BA0。23/23原生、104項Python（無略過）及固定依賴通過；測試包含開啟後修改實際fixture長度／時間／屬性與逐byte BE核對。尚未讀入遊戲shader或達成標題／主選單；下一項是同步實際讀取。詳見 [檔案資訊證據](asset-files.md)。

同步實際讀取／關閉階段接通 NtReadFile 與已知檔案 NtClose。原遊戲將整份93676-byte basic shader讀入40001BA0，從guest緩衝區計算SHA-1 96b652112909d62121e25f2fdb24bfd3b23869d2，與本機資產完全相符；實際CloseHandle後繼續到shader/effect載入825E7F98，因原生device保護停止，進入數1297。主機使用真正同步ReadFile與native位置，支援明示偏移／current、partial／EOF／zero；guest完整輸出及重疊檢查先於host存取，精確複製傳輸bytes並寫BE IO結果。24/24原生、104項Python（無略過）及固定依賴通過。尚無原遊戲draw／Present、標題／主選單或確認輸入；下一項是保留原始effect載入流程並連接必要原生圖形依賴。詳見 [實際資產讀取證據](asset-files.md)。

原始effect入口階段經完整函式及獨立審查，只允許825E7F98接收已知native device，並放行完全不讀r3的純stack-save helper82A56044；沒有替換原始FXOBJ解析，也沒有放行後續device AddRef。真實解析到CreateVertexShader824ED770，container40019B18、596bytes的SHA-1與asset1124及既有轉譯manifest完全一致。下一停止為shader建立內部824D1858（LR824ED808、1361次進入），尚未建立native shader。新增不干擾原始callback的有界唯讀shader容器trace；24/24原生、104項Python（無略過）及固定依賴通過。下一項是原生shader建立適配，標題／主選單仍未達成。詳見 [實際effect解析證據](asset-files.md) 與 [shader intake對應](graphics-shader-intake.md)。


## 原始流程建立全部 basic shader 原生資源

固定 Marathon XenosRecomp `fb32631ee398e46f2a113d8f9103201dbaa000b4` 已納入
可重現工具鏈。受檢查的已知原始資產重新產生68個容器、HLSL與DXIL；來源與生成位元組
仍只保存在忽略目錄。34個mask0的vertex shader直接編成vs_6_0，34個mask2的pixel
shader保留lib_6_3，等待實際遊戲alpha-test狀態，不填入未觀察到的特殊化值。

原始825E4700效果解析逐一收到68個native handles；完整容器位元組與stage比對快取，
原生端保有實際Plume stage物件或DXIL library。handles只保留不可讀頁面，不偽造Xbox
driver標頭。經獨立審核的原始824F44F8僅對已知device放行，執行既有BE參考計數的
遞增。真實入口在1419次函式進入後到825E47D8（LR825E8428），第一個缺失指令是sthu
於825E4954。尚無原遊戲draw/Present、標題畫面或確認輸入。

驗證：`out/native-shaders-final-build.log`；`out/native-shaders-cache-test.log`
證明全部68筆原生資源；`out/native-shaders-addref-boot.log`是真實同次啟動證據；
`out/native-shaders-final-ctest.log`25/25；`out/native-shaders-final-tests.log`
117項Python、無略過；`out/native-shaders-final-dependencies.log`三項固定依賴通過。
Runtime與準備工具均經獨立規格及品質審查；原始AddRef也經獨立逐指令確認。
詳見 [原生shader建立](native-shaders.md)。


## 原始效果初始化通過 sthu 轉譯

`sthu` 的實際原始位元已逐一核對：450筆註記／447個不同位址均匹配opcode45與
運算元，目標825E47D8包含60筆。新的受檢查實作保留完整64位地址更新、低32位記憶體
定址、低16位big-endian存放及失敗前不更新RA，來源／基底同暫存器亦正確。
嚴格產生器只接納具有一致日誌證據的空白標準指令區塊；其他錯誤仍使整個函式停止。

新輸出 `out/recomp/diagnostic-sthu` 保留31,198函式／拒絕15,305，新增84個原始函式及
197處sthu呼叫；72處subfze保持。原始輸入檔雜湊全部未變，沒有新增拒絕函式。
真實啟動 `out/sthu-native-boot.log` 保持68個shader原生資源，原始效果初始化已正常
離開82AD440C臨界區；下一停止為824EC028中的lhzu824EC050，LR824B7DA4、1514次進入。
尚未到達標題或主選單，也尚無原遊戲draw／Present。

驗證：`out/sthu-final-build.log`、`out/sthu-final-ctest.log`（26/26）、
`out/sthu-final-tests.log`（127項Python，無略過）、`out/sthu-final-dependencies.log`。
Runtime與產生器皆經獨立規格／語意及品質審查。詳見 [sthu與真實效果初始化](store-halfword-update.md)。
