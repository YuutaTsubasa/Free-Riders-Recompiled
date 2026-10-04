# 比賽畫面的速度：量到的成本分布

日期：2026-09-21。分支 `claude/function-boundaries`。

## 2026-09-21：Free Race 從 1.6 fps 到 8.4 fps

裝備零件頁通了之後第一次能跑完整的 Free Race，而它比教學關重得多：每格 **650-880
次繪製**（教學關 220-260），一次繪製最多 **64494 個頂點**。實測 **1.6 fps，一格 620 ms**。

### 先量天花板

`SFR_SKIP_DRAWS=1` 原本只在 `native_draw` 開頭返回，而**頂點收集是在那之前**於
`824F56E8` 裡做完的，所以量到的「不繪製」根本沒有排除算繪器。修正成真正在做任何事
之前返回之後：同一段畫面 **53 ms 一格（19 fps）**。也就是說客體模擬只佔 53 ms，
**我們的繪製路徑吃掉 470 ms，九成的一格時間**。

### 逐段計時勝過取樣

主機側取樣把成本歸到 `MoveAbove15`（memcpy 內部，28.7%）。照著優化 —— 依步幅特化
收集迴圈讓 memcpy 內聯 —— 結果**完全沒有效果**，只是把樣本從 `MoveAbove15` 搬到
`sub_824F56E8`（14.6% → 52.9%）。

改用 `steady_clock` 直接量各段：

| | 一格 |
| --- | --- |
| 索引迴圈 | 133 ms |
| 頂點收集 | 176 ms |
| strip 切割 | 0 ms（這一關沒有 primitive restart） |

每個索引 **165 ns**、每個頂點 **200 ns**。一個十二條指令、連續讀 2 位元組的迴圈不可能
如此。`QueryThreadCycleTime` 顯示**實際執行週期與牆鐘相符**，所以不是被搶走核心 ——
每次讀取真的要約 500 個週期。

### 根因：write-combined 記憶體

`MmAllocatePhysicalMemoryEx` 的快取模式 `0x404` 表示 write-combined，遊戲用它配置
D3D 緩衝區（**頂點與索引緩衝就在其中**），一趟執行有 **1480 筆**。`guest_memory.cpp`
忠實地把它們對映成 `PAGE_WRITECOMBINE`。

在主機上這是對的：那些記憶體只有 GPU 會讀。在這裡**讀它的是 CPU**（算繪器要把資料
複製進上傳環），而 write-combined 記憶體的**讀取完全不經過快取**，每次都上 DRAM。

改成預設用一般快取頁面（`GuestMemory::set_host_write_combining`，`SFR_WRITE_COMBINED=1`
還原）。這純粹是主機端的快取選擇：x86 的快取記憶體**排序比 write-combined 更嚴格**，
遊戲依賴的順序保證不會被削弱。

### 另外：索引繪製不再逐頂點收集

`824F56E8` 改成掃出索引用到的最小／最大值，把串流的 `[min..max]` **一次整塊上傳**，
索引減去 `min` 之後交給 GPU 做索引（`setIndexBuffer`／`drawIndexedInstanced`）。
索引散得太開（區塊超過實際用量四倍）或含 primitive restart 的繪製仍走收集路徑。
繪製用的頂點與索引改放在**會保留容量的執行緒區域暫存區**，不再每次繪製配置 vector。

### 結果

| 同一段比賽畫面 | 一格 | fps | 索引迴圈 | 頂點收集 |
| --- | --- | --- | --- | --- |
| 原本 | 620 ms | 1.6 | 133 ms | 176 ms |
| 索引緩衝 | 465 ms | 2.2 | 157 ms | 93 ms |
| 加上快取對映 | **117 ms** | **8.4** | **1.1 ms** | **1.6 ms** |

畫面以截圖驗證過：比賽場景、角色、HUD 都正確，比賽正常推進。

### 再一輪：每次繪製重複做的固定工作

剩下的一格 117 ms 裡，逐段計時顯示 `native_draw` 本身佔 **37 ms**（857 次繪製，
每次 43 µs），而其中常數複製只有 1.1 ms、指令錄製 3.4 ms。`present_ms` 只有 **6.7 ms**
—— 這台機器是 RTX 4090，**完全不是 GPU 受限**。那 37 µs 是四件每次繪製都重做一遍的事：

1. **用例外當控制流**。把 GPU 實體位址換成可讀的虛擬位址時，依序試三個視圖，
   失敗的那幾次靠 `catch (const RuntimeStop&)` 接住。`RuntimeStop` 帶兩個
   `std::string`，在 Windows 上丟一次要好幾微秒，一格丟上千次。改用不丟例外的
   `GuestMemory::readable`。
2. **每次繪製重新驗證貼圖**：`take_written` 要逐頁走過整張貼圖的髒頁位元，而且三個
   視圖各走一次。改成每格驗證一次（旁邊的內容雜湊檢查本來就是這個行為）。
3. **骨架調色盤**每次繪製配置一個 vector 並換最多 16 KB；改用保留容量的暫存區，
   而且只換實際複製的部分。
4. **`NativeDraw` 每次重建**：兩個各 4 KB 的常數陣列被零初始化、元素清單重新配置。
   改成重複使用，只明確重設四個繪製可能不會寫到的欄位。

同規模的選單畫面：每次繪製 **21 µs → 4.9 µs**。

### 目前的狀態

| 同一段比賽畫面 | 一格 | fps |
| --- | --- | --- |
| 最初 | 620 ms | 1.6 |
| 索引緩衝 | 465 ms | 2.2 |
| 加上快取對映 | 117 ms | 8.4 |
| 加上拿掉每次繪製的固定工作 | 97 ms | 10.3 |
| 加上關掉每次進入的診斷 | 85 ms | 11.7 |
| 加上 DEC3N 查表、管線鍵、WC 頁面表 | **53 ms** | **18.8** |

一格 85 ms 的組成：客體模擬約 **50 ms**、繪製路徑約 **27 ms**（索引 1.3、頂點 1.6、
`native_draw` 23.8）、present 約 **9 ms**。也就是說**算繪器已經不是主要成本**，
即使讓它完全免費也只到約 16 fps。要再往上必須動客體側：每次記憶體存取的界限檢查、
`enter_function`／`guest_checkpoint` 這些診斷檢查點，以及客體執行緒共用單一執行許可。

比賽畫面裡每次繪製仍有約 24 µs（選單只有 4 µs），差別來自貼圖數量、骨架調色盤與
頂點宣告的規模 —— 那是下一個可以往下拆的地方。

### 主執行緒在排隊：單一執行許可的代價

側寫的執行緒分布是主執行緒約 68%、**執行緒 29（工作系統的背景工作執行緒，
`0x8222E008`）約 26%**。所有客體執行緒共用一個執行許可，所以要知道它跑的時候
主執行緒是在等它的結果，還是明明能跑卻被許可擋住。`GuestExecution` 現在記錄
主執行緒（客體 1）「就緒但在排隊」的時間，印在 present 行的 `main_queued_ms`：

**比賽中平均每格 17.5 ms**（一格約 45-50 ms）—— 三分之一的一格純粹是被序列化吃掉的。

試過而且失敗的：把主執行緒設成緊急（`execution.set_urgent(1, true)`），讓它就緒時
在擁有者的下一個檢查點搶回許可。**選單中死結**（第 5092 格）：一個客體對另一個客體
在等的事件呼叫 `NtSetEvent` 之後，所有執行緒都停在等待，5 秒只用 0.6 秒 CPU。同一個
exe 只關掉這個開關就順利通過，所以確定是它。現在是 `SFR_MAIN_URGENT=1` 才開啟的實驗。

要拿回這段時間，真正的做法是讓客體執行緒並行，但 `GuestMemory` 的 `lwarx`／`stwcx.`
保留狀態與許多 HLE 狀態都假設同一時間只有一條客體執行緒，是一個大工程。

另外 `swap_words` 改成 SSSE3 一次換 16 位元組（側寫佔主執行緒 7%），但同一段畫面
看不出明確差異，在 17-20 fps 的雜訊範圍內。

執行緒 29 最熱的函式幾乎全是 VMX128 向量運算（`lvx128`、`vmaddfp`、`vspltw`）——
矩陣／向量變換，是真正的遊戲工作。對齊的 16 位元組向量存取改走快速路徑
（`GuestMemory::fast_read`／`fast_write`：一次頁面檢查、直接反轉 16 位元組），
原本每次都跑兩輪檢查再逐位元組組合。同樣看不出明確差異：17-20 fps、主執行緒排隊
16.2 ms。**單執行緒的微調到這裡已經邊際遞減，剩下的是執行許可本身。**

### 再一輪：側寫點名的三個每次存取成本

關掉進入點診斷之後重新側寫主執行緒：

1. **DEC3N 法線**每個頂點每個分量呼叫一次 `std::lround`：`lroundf`＋`roundf`＋
   `_fdtest_inline` 合計約一成。10 位元的輸入只有 1024 種值，改成用同樣捨入方式
   預先算好的查表。
2. **管線鍵**每次繪製對每個語意名稱做 `strlen`，而且鍵本身是每次繪製配置的
   vector。名稱全都來自 `declaration_semantic` 的靜態字串表，所以改存位址，並用
   保留容量的緩衝區。
3. **`intersects_write_combined`** 對遊戲建立的約 1500 個 write-combined 區段做
   線性掃描，而每個 `ldarx`／`stdcx.` 和每個**走慢路徑的儲存**都會問一次。改成每頁
   一位元組的旗標表：一般情況直接回答「不是」，只有真的落在 WC 頁面時才做精確比對。

同一段比賽畫面：**85 ms → 53 ms，11.7 fps → 18.8**。繪製路徑從 21 ms 降到 11 ms，
其餘來自客體側 —— 那個區段掃描原本在每一次沒走快速路徑的儲存上。

### 再一輪：每次客體函式進入的診斷

`enter_function` 在**每一個客體函式進入時**都會跑，而內容幾乎全是觀測：一長串
`ORIGINAL_*` 稽核、進入追蹤、`SFR_DUMP_ENTRY`／`SFR_TRACE_ENTRY`／`SFR_WATCH` 等
除錯開關（每個 `static const` 都帶一次初始化守衛檢查），以及側寫用的取樣位址。

現在無條件做的只有 `guest_checkpoint()`（客體執行緒排程的交接點，不能拿掉）和記下
函式名稱與位址，其餘由 `SFR_DIAGNOSTIC_ENTRIES` 控制。**預設開啟**（測試與那些稽核
都靠它），`scripts/play.ps1` 關掉；`SFR_SAMPLE_PROFILE`／`SFR_HOST_PROFILE` 會強制開啟。

關掉之後同一段比賽畫面：**97 ms → 85 ms，10.3 fps → 11.7**。

過程中被兩個「其實不是觀測」的東西擋下來，兩個都是真正的耦合：

1. **未選取使用者的證據**：`enter_function` 收集的旗標由**函式外面**一個稽核檢查，
   關掉就中止（`STOP original-user-reset`）。那兩段移到閘門之前，成本接近零 ——
   兩個判斷都以 `unselected_user.active` 開頭，稽核沒在進行時第一個比較就短路。
2. **`SFR_ALLOW_RENDER_TARGETS` 本身**：決定比賽畫不畫得出來的
   `GuestGraphics::foreign_render_targets`，是靠 `enter_function` 裡一個
   `static const` 的初始化**副作用**設定的。關掉就永遠不會初始化，比賽一開始綁自己的
   繪製目標就 `STOP native-graphics-state: unknown viewport attachment`。已改成在
   程式啟動時初始化 —— 這種旗標不該依賴「有沒有客體函式進入過」。

### 客體執行緒並行（`SFR_PARALLEL_WORKER`）

單一執行許可的代價量得到（主執行緒每格排隊十幾 ms），做法是讓客體執行緒真的並行，
但只在碰到主機狀態的地方拿許可：

1. **保留狀態改成每條主機執行緒各一份**（`guest_reservation`，thread_local，以
   不重複的記憶體 id 為鍵），`stwcx.`／`stdcx.` 用對保留值的 compare-and-swap
   寫入。四條執行緒同時 `lwarx`／`stwcx.` 遞增的測試結果精確。
2. **`GuestExecution::Lease::detach()`／`attach()`**：分離的客體跑客體程式碼時不佔許可，
   檢查點只看取消；要碰主機狀態前 `attach()` 排隊拿回許可。
3. **哪裡要拿許可**：import（臨界區段除外 —— 它有自己的鎖，只有競爭時的等待才
   attach）、我們的 hook（現在用 `SFR_HOOK` 宣告，執行期知道哪些位址是 hook；進入
   hook 就 attach，直到在比最外層 hook 的堆疊框更上層的地方進入函式才放開）、以及
   計算字（provider 是主機函式）。
4. **記憶體配置**（已提交區段、import 變數、pending I/O）只在持有許可時改變；分離的
   執行緒是 `GuestMemory::concurrent_reader`，慢路徑在共用鎖下讀，改變的一方取獨佔鎖。
   被監看頁面的旗標改成原子操作。

`SFR_GUEST_REACH=1` 印出每條非主執行緒（第一次）碰到的函式與 import：比賽中工作
執行緒 29 沒碰任何 hook、只用四個 import，這是先從它開始的依據。

`SFR_PARALLEL_WORKER=1` 只分離工作執行緒（`0x8222E008`）；`=all` 分離主執行緒與
音訊幫浦以外的所有客體執行緒。

### GPU 落後一幀

加上 `main_blocked_ms`（主執行緒在等待裡的時間）與 `gpu_wait_ms` 之後看到：主執行緒
每格有 **8.3 ms 在等 GPU** —— present 送出這一格後就等 GPU 畫完，CPU 與 GPU 從不重疊。
現在 present 把複製到交換鏈的指令接在這一格自己的指令清單後面，送出後不等；下一格
錄進第二份清單，送出時才等還在飛的那一格（多半早已畫完）。算繪器有兩個上傳環，
一格釋放的貼圖槽要等那一格畫完才回收（`after_flush(complete)`）。
`SFR_GPU_PIPELINE=0` 回到等待的 present。管線化前後同一格的截圖完全相同。

### 結果（2026-09-21，比賽中每格都畫）

| 同一段比賽畫面 | 一格 | fps |
| --- | --- | --- |
| 單一執行許可 | 45 ms | 22 |
| 分離工作執行緒 | 39 ms | 26 |
| 加上 GPU 落後一幀 | 32 ms | 31 |
| 加上分離所有客體執行緒 | **28.8 ms** | **35** |

28.8 ms 的組成：主執行緒持有許可約 26 ms（其中繪製路徑約 7 ms），排隊 4.6 ms
（許可交接的喚醒延遲，別的執行緒實際持有不到 1 ms），等待 3 ms。`holders=` 顯示每格
持有許可最久的客體。

**整場 Free Race**（`SFR_PARALLEL_WORKER=all`、`SFR_RENDER_EVERY=2`）：比賽中約
40 game fps，從開始到 GOAL **6.5 分鐘**（之前 9.9 分鐘；遊戲內時間 4'27"），開機到
比賽開始 3.8 分鐘，之後正常進到結果後的環狀選單。

### 再一輪：主執行緒的每次進入與每次存取

`SFR_MAIN_PROFILE=1` 只取樣主執行緒（不打開進入點診斷），用連結器的 map 檔換成
符號：

1. `enter_function` 每次把函式名稱**複製**進 thread_local `std::string`（strlen＋複製）
   —— 改存字面值的指標。
2. `guest_checkpoint` 每次函式進入與迴圈都呼叫許可 —— 改成每 32 次一次，倒數放在
   `diagnostic_hooks.h` 裡內聯。
3. 管線快取是 `std::map<vector>`，每次繪製逐位元組比較約 100 位元組的鍵 —— 改成雜湊。
4. 主執行緒的受檢路徑存取集中在兩個字：XEX import 頁上的 `0x8200092C` 與 TLS 區塊
   `0x71300020-4C`（不滿一頁的提交）。每次 `check()` 都**線性掃描所有已提交區段**
   （上千個）—— `committed_` 本來就排序合併，改成二分搜尋。
5. `load`／`store` 的快速路徑改成內聯的 volatile 位元組交換存取，受檢路徑另成函式。

28.8 → 26.4 → **23.7 ms**。

### 每個核心一張許可（`SFR_PARALLEL_WORKER=cores`）

`all` 模式跑整場時，比賽一開始客體執行緒 36 呼叫了 CRT 的 `_purecall`（R6025 pure
virtual function call，停在 `RtlInitUnicodeString`）。worker 停止時現在會印出客體呼叫
鏈，追到工作分派 `823B5D40`：它把項目放進無鎖佇列（`'LfQu'`），喚醒三條輔助執行緒
（`823B60C0`，處理器 1、4、5），自己也一起消化，然後
`WaitForMultipleObjects(3, done, TRUE, 16 ms)` **不看結果**就重設、重用項目。

兩個問題：

1. **同一個硬體執行緒上的執行緒在主機上同時跑**：主機不會這樣排程。客體執行緒的
   處理器在 PCR+0x10C（`KeSetAffinityThread` 設定）。`cores` 模式讓處理器 1-5 的執行緒
   脫離全域許可，但執行客體程式碼時要持有**該核心的許可**（每核心一個
   `GuestExecution`，同樣 2 ms 輪替）；處理器 0 的執行緒（包括主執行緒）留在全域許可。
   只在沒拿全域許可時等核心：阻塞等待會連核心一起放掉（`Lease::wrap_blocking`），
   拿著全域許可時不在核心上讓出，避免交叉等待。
2. **16 ms 的等待是遊戲本身的時序假設**：只讓工作執行緒並行（`=1`）時也出現同樣的
   R6025（這次在序列化的客體 14）。主機上的輔助執行緒可能超過 16 ms（程式碼較慢、等
   許可）。`game_patches.cpp` 在這個呼叫點（`824D0B10`，LR `823B5EA0`）把逾時改成
   `SFR_WORK_SHARE_WAIT_MS`（預設 1000）並回報逾時。保留有限值：輔助執行緒若在下一格
   的 resume 之後才自我暫停，要靠下一格的等待逾時放行。

| 同一段比賽畫面（每格都畫） | 一格 | fps |
| --- | --- | --- |
| 分離所有客體執行緒（`all`） | 23.7 ms | 42 |
| 每核心許可（`cores`） | **20.6 ms** | **48.5** |

`cores` 反而更快：主執行緒排隊從 4 ms 降到 0.8 ms（多數執行緒改在各自的核心許可上
輪替，不再搶全域許可）。

**整場 Free Race**（`cores`、`SFR_RENDER_EVERY=2`）：開機到比賽開始 **3.5 分鐘**，
開始到 GOAL **4.6 分鐘**（遊戲內 4'27"，約 60 game fps，幾乎即時），進到結果後的環狀
選單，沒有等待逾時。`scripts/play.ps1` 預設 `cores`（`-Serial` 關閉並行）。

### 再一輪：誰讓脫離的執行緒回到全域許可

`holders=` 顯示客體 16 每格「持有」許可 15 ms，但它 93% 的取樣在等待：它**拿著核心
許可排隊等全域許可**（進 import 或 hook 時），同一核心的執行緒全被擋住。

1. **順序改成全域先於核心**：`Lease::set_companion` —— 等全域許可之前（attach、
   run_blocking、檢查點的讓出）先放掉核心，拿回全域後再拿核心。持有核心的執行緒永遠
   不等全域許可，所以不會死結。第一版在 attach 狀態下不讓出全域許可，開機 16 格就卡死
   （hook 裡的遊戲程式碼在等主執行緒）；改成讓出時一樣先放核心才解決。
2. **不必拿許可的 import**：`PARALLEL_IMPORTS` 列出讓脫離執行緒回到許可的 import。
   `KeTlsGetValue` 一分鐘四十萬次（值在呼叫者自己的 TLS）；等待、事件、睡眠、號誌、
   程序類型、音量也不需要：`NativeSyncObjects` 自己上鎖，`dispatcher_mutex` 保護客體
   端的 DISPATCHER_HEADER，等待只放掉核心（`block_guest`）。核心許可跟著全域許可停止
   （`add_follower`），關機時不會有執行緒卡在核心上。
3. 記憶體池配置 `824A3398` 的 hook 試過改成不拿許可（`SFR_CONCURRENT_HOOK`），結果
   出現被破壞的指標，已改回。

`SFR_WAIT_GRAPH=1` 顯示主執行緒剩下每格約 2 ms 的等待是在等**同一處理器（0）上的客體
7**：主機上它們本來就不能同時跑，這段是遊戲本身的結構。

### 再一輪：主執行緒自己的工作

| 改動 | 一格 |
| --- | --- |
| 起點（`cores`，每格都畫） | 20.6 ms |
| 頂點直接換位元組寫進上傳環（原本複製、原地換、再複製三趟） | 20.6 ms |
| 同步 import 不拿許可、全域先於核心 | 19.3 ms |
| `enter_function` 快速路徑內聯 | 18.9 ms |
| 索引 SIMD 解碼（1.05 → 0.32 ms），用 base vertex 取代逐一減去最小值 | 18.9 ms |
| 每個貼圖槽記住上次的查找結果（`texture_generation` 變動才重查） | 18.5 ms |
| DEC3N 頂點直接打包進上傳環 | **17.8 ms** |
| 只檢查用到的頂點範圍；TLS 整頁提交；import 變數頁的一般讀取不走受檢路徑 | 17.8 ms |

主執行緒省下的時間有一部分變成等客體 7，所以不是每一項都反映在一格時間上。

同一幀內重複上傳的頂點資料只有 0.8 MB（總共 20.9 MB），單幀去重沒有價值；剩下最大
的單項是頂點換位元組（約 9%），要再省只能做跨幀的頂點快取。

### 比賽載入：59 秒 → 13 秒

腳本跑法下，開機到選單剛好 60 fps（選單有限速、輸入腳本按幀數定時），真正受模擬速度
影響的是比賽載入（第 9600-10200 格約 59 秒）。取樣這段主執行緒，22% 在
`GuestMemory::lowest_conflict`／`available`：實體記憶體配置找空位時要探測上千次，每次
都線性掃描上千個保留區。保留區改成依位址排序、二分搜尋之後，**載入約 13 秒**。

另外檔案讀取原本逐位元組寫進客體記憶體（非同步讀取每個位元組都走一次完整檢查），
改成整段檢查一次再複製（`write_bytes`）；在這段量測裡看不出差異，但本來就不該逐位元組。

**整場 Free Race，`scripts/play.ps1` 的預設（`cores`、每格都畫）**：腳本跑法開機到比賽
計時開始約 3.1 分鐘（大多是腳本按幀數定時的選單），開始到終點約 **4.8 分鐘**（遊戲內
4'24"；今天稍早同樣設定 5.8 分鐘），進到結果後的環狀選單，沒有停止或等待逾時。

### 跨幀頂點快取（`SFR_VERTEX_CACHE`）與 60 fps 上限

大部分頂點是關卡幾何，每格都一樣。`NativeRenderer::vertex_cache`：從 GPU 緩衝區就地讀的
頂點（已知實體位址）先監看它所有的虛擬位址對應；經過一整格沒被寫入就放進自己的
upload-heap 緩衝區，之後沒被寫就直接用；一被寫就丟掉（緩衝區留到用它的那一格畫完）。
緩衝區填好後不再修改，所以不會和還在畫的那一格衝突。

寫入偵測用 `GuestMemory` 的寫入 epoch：被監看頁面的儲存記下目前的 epoch（每格前進），
`written_since` 回答一段範圍是否在某個 epoch 之後被寫過。和貼圖快取用的
`take_written` 位元不同，多個擁有者可以問同一頁。

比賽畫面 **17.8 → 16.8 ms**（727 次繪製中 377 次用快取），整場比賽每張截圖都正確
（包括結果表與選單）。`scripts/play.ps1` 預設開啟（`-NoVertexCache` 關閉）。

遊戲在比賽中每格固定前進 1/60 秒、不等畫面更新，超過 60 fps 就會比實際時間快
（每兩格畫一格時曾到 79 fps）。`SFR_FRAME_LIMIT=60` 讓 present 至少間隔 1/60 秒，等待時
放掉執行許可；預設關閉（測試要快），`scripts/play.ps1` 開啟。

**整場 Free Race，`scripts/play.ps1` 的預設**：比賽約 59-60 fps，開始到終點約
**4.4 分鐘**，遊戲內 4'18"，也就是接近即時。


---

## 2026-09-20：教學關的舊量測

比賽可以玩之後第一件被指出的事就是慢。教學關比賽中約 **7-9 fps**（每格 220-260 次
繪製，1280×720），同一段畫面在不同時間點的差距也有 6.2-9.5 fps，所以任何比較都要
比**同一段畫面**。

## 量法

`SFR_SAMPLE_PROFILE=1`（原名 `SFR_PROFILE`，後來改名，因為 `SFR_PROFILE` 成了登入帳號的開關）每毫秒取樣「該執行緒最後進入的客體函式」，`SFR_HOST_PROFILE=1` 取樣
主執行緒的主機指令位址（用 `out/build/host/sfr_cpu_diagnostic.map` 解析）。

客體側（290774 個樣本）最上面幾項：

| 樣本 | 位址 | 是什麼 |
| --- | --- | --- |
| 111816 | `0x824F56E8` | DrawIndexedVertices（我們的原生繪製） |
| 21091 | `0x824DF2B8` | 工作執行緒 |
| 15425 | `0x824FAB08` | Resolve |
| 11274 | `0x824E65A0` | Present |

主機側（237831 個樣本）：

| 比例 | 位置 |
| --- | --- |
| 25.5% | 映像外（D3D12 執行期與驅動，含 GPU 等待） |
| 20.6% | `memcpy`（`MoveAbove15`／`MoveWithYMM`／`Mov2YmmBlocks`） |
| 7.1% | `sub_824F56E8` 本身（逐索引收集頂點的迴圈） |
| 12.4% | `GuestMemory` 的 `lowest_conflict`／`available`／`check`（每次客體存取） |
| 2.8% | `content_hash`（動態貼圖每格比對） |
| 4.3% | `enter_function`／`guest_checkpoint`／`Lease::checkpoint`（診斷用的檢查點） |

## 已經做的

- **逐呼叫的圖形追蹤可以關掉**（`SFR_TRACE_GRAPHICS=0`，`play.ps1` 預設關）。原本每格
  約 5000 行（其中 2500 行是 `NATIVE_TEXTURE_BIND`），十分鐘寫 2.1 GB。同一個建置、
  同一段畫面的 A/B：開 7.95 fps、關 8.2 fps，**只差約 3%**——記錄量雖大，但格式化的
  成本沒有想像中高；關掉主要是為了不要再寫出 GB 級的檔案。
- **著色器常數整塊複製**：原本每次繪製用 2048 次逐字載入把 VS/PS 的 c0..c255 搬過來
  （每次都經過界限檢查），改成一次 `check` + `memcpy` + 整塊換位元組。
- **調色盤常數緩衝不再每次繪製清零**：只寫實際有的項目，省下每次繪製 16 KB 的 memset。
- **Resolve 的複本錄進當格的指令串列**：原本每次 resolve 都 `flush()` 送出並等 GPU，
  一格十幾次來回；現在照順序錄在同一個串列裡，不再等待。

這幾項加起來在同一段畫面上看不出明確差異（落在 6.2-9.5 fps 的變動範圍內），真正的
瓶頸還在下面。

## 上限：算繪完全免費也只有 12-13 fps

`SFR_SKIP_DRAWS=1` 讓每次繪製在做任何事之前就返回（畫面只剩清除），量同一段比賽畫面：

| | 畫面 4200-4500 | 4500-4800 | 4800-5100 | 5100-5400 | 5400-5700 | 5700-6000 |
| --- | --- | --- | --- | --- | --- | --- |
| 正常 | 7.9 | 7.1 | 6.9 | 9.0 | 9.0 | 9.3 |
| 不繪製 | 13.2 | 12.0 | 11.5 | 12.4 | 12.4 | 13.1 |

也就是說**整個原生算繪器佔一格的三分之一到四成**，其餘六成是重編譯的遊戲程式碼加上
我們的 HLE。即使算繪器快到不花時間，這一段比賽也只有 12-13 fps。要更快就得動客體側：

- **客體執行緒是輪流跑的**：`GuestExecution` 只有一個 permit（2 ms 的時間配額），遊戲的
  多個執行緒全部擠在一個核心上。取樣顯示主執行緒之外的執行緒佔了兩三成的 permit 時間
  （其中 `0x82A5606C` 之類的等待／自旋就有 6%）。
- **每次客體記憶體存取都做界限檢查**（`lowest_conflict`／`available`／`check` 共 12%）。
  改成把客體記憶體平坦對應到主機位址空間、用保護頁處理越界，就幾乎不用檢查。
- **診斷檢查點**（`enter_function`／`guest_checkpoint`／`Lease::checkpoint` 共 4.3%）。

## 試過而且失敗的：整份頂點緩衝加索引緩衝（2026-09-21 已由區塊上傳取代）

把 `824F56E8` 改成上傳整個頂點串流加一份索引緩衝（`setIndexBuffer`／
`drawIndexedInstanced`），想省掉逐索引的複製。結果比賽掉到 **0.7 fps**：遊戲綁的是
**很大的共用頂點串流**，每次繪製只畫其中一小塊，所以上傳整份的資料量遠大於只收集用到
的頂點。已經回復。

要走這條路必須先有**每格一次的頂點緩衝快取**（以緩衝的實體位址為鍵，一格之內多次
繪製共用同一份上傳），還要處理遊戲在同一格內改寫緩衝的情況（`GuestMemory::watch_writes`
已經有這個機制，貼圖快取在用）。沒有快取就不要再試一次。

## 還沒做的（依預期效益）

1. **每格一次的頂點緩衝快取**（見上）：這才是真正能動到那 28% `memcpy` 的做法。
2. **換過位元組的頂點資料跨繪製快取**：同一格裡多次繪製常常用同一個頂點緩衝。
3. **常數只在變動時上傳**：現在每次繪製都上傳 8 KB。
4. ~~**診斷檢查點**~~：2026-09-21 做了，見上面 `SFR_DIAGNOSTIC_ENTRIES` 那一節。

## 2026-09-23：尖峰而不是平均

同一段比賽（`cmp.sh`，每格都畫、上限 60fps）的每格時間中位數是 **17.0 ms**，也就是
已經貼著 60fps 的上限；真正影響體感的是**尖峰**。`NATIVE_PRESENT` 因此多了四個計數：
`pipelines` / `pipeline_ms`（管線建立）、`ring_flushes`（上傳環中途滿了）、
`textures` / `texture_ms`（貼圖上傳與查找）。

量出來的結果：

| 嫌疑 | 實際 |
| --- | --- |
| 管線建立 | 整場 952 個、合計 **50 ms**，尖峰格只佔 1–2.6 ms —— 不是主因 |
| 上傳環中途 flush | **0 次** —— 不是主因 |
| 貼圖 | 最慢的 40 格裡 `draw_ms` 7.69 ms 中有 **3.47 ms** 是貼圖 |

每次貼圖上傳約 **1.3 ms**，因為它提交一個獨立的命令清單後**等待 GPU 完成**。同一佇列
的提交本來就依序執行，所以那個等待是多餘的；改成不等、並用四個命令清單輪替，只有繞
回同一個槽時才等它的 fence。

這條路上踩了兩個坑，兩個都是**正確性**問題（見下）：

> 1. 只用一個命令清單而不等待：GPU 還在讀它的時候就被重新錄製，整場比賽的貼圖會變全黑。
> 2. 暫存緩衝區按「兩格之後」釋放：GPU 還沒讀完就被回收，**整個遊戲會死鎖**
>    （docs/permit-deadlock.md）。正確的做法是讓每個上傳槽自己持有它的暫存緩衝區，
>    只有在等到該槽的 fence 之後才釋放。

| 同一段比賽 | 之前 | 之後 |
| --- | --- | --- |
| 最慢 40 格的 `draw_ms` | 7.69 ms | **7.02 ms** |
| 其中貼圖 | 3.47 ms | **2.09 ms** |
| 整場最差的一格 | 41 ms | 39 ms |

貼圖的部分省下四成，但最差的一格只好一點點：那一格的成本並不只在貼圖。

### 接下來還剩什麼

一格 17 ms 裡，主執行緒持有許可約 **13 ms**、真正等待約 **3 ms**（`SFR_WAIT_GRAPH=1`
顯示是在等客體 7、8、9 這三條遊戲自己的工作執行緒）。那 13 ms 裡渲染器只佔約 5.8 ms
（繪製 3.3、錄製 1.5、索引與常數約 1.0），其餘是遊戲自己的程式碼。也就是說**渲染器
已經不是主要成本**，再往下要動的是重編譯程式碼本身或排程。


## 2026-09-24：Android 核心分配與執行緒資料存取

裝置為 AYANEO Pocket S2 Pro（Android 14、Adreno 750）。圖形已使用
`v8` shader cache 的 push-constant 位址修正；以下測試保留相同 shader pack。

### 子執行緒被父執行緒的 CPU affinity 困住

POSIX `NativeThread` 建構時用 `sched_getaffinity(0, ...)` 作為可用 CPU
清單。這裡的 0 指呼叫執行緒：遊戲先把父工作執行緒釘到 CPU 6，再從它
建立子執行緒時，子執行緒只得到 CPU 6。後續即使遊戲要求不同客體核心，
`set_guest_processor()` 仍只能選 CPU 6。

修正為讀取程序主執行緒的 affinity（`getpid()`）；本程式的程序主執行緒
不參與客體核心綁定。核心速度排序與個別執行緒綁定保持原有行為。

回歸測試 `native_thread_posix` 先取得六個客體核心的預期映射，再從已綁定
的父執行緒建立子執行緒，驗證子執行緒仍能選擇相同映射。實機修正前：
`guest_cpu=0 expected=128 actual=64`；修正後六個映射全部通過，依序為
`128, 4, 8, 16, 32, 64`。遊戲紀錄也確認客體 29 的 affinity 從錯誤的
`0x40` 變成 `0x20`。

Free Race 的末段 101 格樣本，先前 `SFR_PARALLEL_WORKER=1` 約 6.53 FPS；
修正 affinity 並使用 `cores` 後約 8.2–10.4 FPS。後一趟末段中位數：
主執行緒排隊 23.38 ms、繪製 10.58 ms、GPU 等待 0.16 ms。
這些是同一賽道的不同時間片段，不能當成完全相同畫面的嚴格 A/B。
`cores` 的開場停住曾經重現，修正後的成功啟動也不足以證明所有時序問題消失。

### Android API 28 建置使用 emulated TLS

NDK simpleperf 實機比賽取樣 15 秒、11252 筆樣本，沒有遺失：
`__emutls_get_address` 14.89%，`pthread_getspecific` 6.79%。
這是 CPU 執行樣本占比，不是整格牆鐘時間占比。

[Android 官方說明](https://android.googlesource.com/platform/bionic/+/HEAD/android-changes-for-ndk-developers.md#elf-tls-available-for-api-level-29)
指出 API 29 起支援 ELF TLS，NDK r26 起會依最低 API 自動選用。預設
Android 9 支援仍保留；可用 `SFR_ANDROID_API=29 scripts/build_android.sh ...`
建立 Android 10 以上版本。腳本同時把 API 傳給原生編譯和 APK 包裝，
避免套件聲稱支援無法載入其原生函式庫的舊 Android。
手動包裝時須指定與原生建置相符的 `--min-sdk 29`。

取樣用 APK 暫時加入 `<profileable android:shell="true" />`，正式 manifest
沒有保留此變更。原始資料與報告存於 `out/android-performance/`。


### API 29 實機結果與交付版本

同一賽道，依 `NATIVE_PRESENT draws>600` 篩出比賽格並對齊序號；各段取
101 筆呈現時間，以 100 個間隔計算 FPS：

| 比賽樣本索引（從 0 起） | 原版 worker=1 / API 28 | affinity 修正、cores / API 28 | 再加 API 29 原生 TLS |
| --- | ---: | ---: | ---: |
| 100–200 | 7.35 | 10.19 | 11.67 |
| 200–300 | 6.54 | 8.53 | 9.42 |

兩段三個版本的繪製次數中位數分別都是 796、793。這比比較任意末段更接近
相同遊戲進度，但仍沒有固定錄影重播或控制裝置溫度，應視為約 44–59% 的
本次測量改善，不是普遍保證。API 29 最後 101 格樣本為 11.90 FPS。

API 29 比賽取樣 11062 筆、無遺失；主要 TLS 成本改為
`[linker]tlsdesc_resolver_dynamic`（11.37%）。原生 TLS 仍有成本，不能把
原本約 22% 當成全部消除。下一批 CPU 熱點包含 hook 查詢、函式進入檢查、
記憶體存取與排程；目前尚未達到流暢的 30/60 FPS。

交付 `out/android/FreeRidersRecompiled-adreno-performance.apk`（arm64-v8a，
minSdkVersion 29）。已安裝在裝置，移除取樣 manifest、暫時的 watchdog
與自動啟動設定。實測取樣 APK 與交付 APK 的 `libmain.so` SHA-256 相同：
`008e4105dd740e5eecf66752eb9e5ef26eff33b65c051c0295d38ea006d94267`。
shader pack 維持原圖形修正版，SHA-256 為
`d430c0c0bff5f7b9937a16d67dd3b5059bc9165296a440a73b32a9201fd93b4a`。

驗證：Android API 28 / 29 的子執行緒核心映射測試通過；Windows 的
`guest_execution`、`native_thread`、`vulkan_shader_source` 三項回歸測試
通過。新版本在裝置通過開場、選單、校正與比賽，截圖為
`out/android-performance/tls-race.png`。保留 `SFR_SKIP_MOVIES=1`、
`SFR_MOVIE_ALLOWANCE_MS=1`，並將 `SFR_PARALLEL_WORKER` 設為 `cores`。


## 2026-09-24：固定的除錯監視器查詢不再取得全域執行權

`SFR_PARALLEL_TRACE=1` 顯示背景執行緒頻繁讀取 `0x820007D4`
（`KeDebugMonitorData`），單一執行緒累積超過 131072 次。在此移植環境，
它永遠指向 `AbsentDebugMonitor::address`，而該位址永遠回傳零。
先前這兩個固定值仍被視為任意主機 callback，每次讀取都可能將背景
執行緒重新接回全域執行許可，直到下一個函式進入點才釋放。

`GuestMemory::ProviderAccess::concurrent` 現在讓經確認可並行的 provider
明確選擇免除該次執行許可取得；預設仍是 `exclusive`。只將上述兩個固定
provider 標成 concurrent，時鐘與裝置狀態等其他 provider 保留原有行為。
記憶體界限、唯讀保護與 byte-order 規則沒有放寬。

為避免在 callback 或取得全域執行權時持有記憶體版面鎖，讀取先於版面
共享鎖內擷取相交 provider 的位址，再解鎖並呼叫。最多八位元組的純量
讀取會跨三個對齊 word。註冊表預留既有的 16 個名額，且 provider 在
`GuestMemory` 存活期間不移除，因此指標不會因追加註冊失效，mutable
callback 的狀態也不會因複製而遺失。

回歸測試先重現 absent-monitor 查詢不必要地要求獨占執行而失敗，修正後
通過。另驗證三個 word 的混合存取、預設仍需執行許可、partial read、
寫入拒絕、mutable callback 狀態，以及 callback 中追加多個註冊項目。
Windows 的 `guest_memory`、`debug_monitor`、`timestamp_bundle`、
`guest_execution` 四項通過；Android API 28 編譯的 memory / monitor
測試也在裝置通過，這項最佳化本身不需要提高 Android 版本。

新版本沿用先前的 1 ms 影片跳過設定時，兩次在第一段影片結束後停止
呈現；恢復原本的 10000 ms allowance（同時仍可在呈現 31 格後跳過）
後曾通過開場並進入比賽，但正式套件再次重啟仍在呈現 31 格後停住
（`restart-stall.log`）。因此不能將 1 ms 判定為唯一原因，也不能將恢復
10000 ms 當成開場問題的修復。裝置交付設定會移除 1 ms override。

本輪原始紀錄保存在 `out/android-performance-round2/`，比賽量測方式
沿用前一輪的 `draws>600` 樣本序號，`measure.py` 可重算結果。

### 第二輪比賽量測

同一台裝置、同一賽道，兩版均使用 API 29、`cores` 與平行追蹤。
本輪重新取得的前版紀錄為 `baseline-trace.log`，新版為 `after-trace.log`。
各段同樣取 101 筆時間戳、100 個間隔；排隊時間為該段中位數。

| 比賽樣本索引 | 前版 FPS | 新版 FPS | 改善 | 前版主執行緒排隊 | 新版主執行緒排隊 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 100–200 | 10.71 | 13.80 | 28.8% | 34.82 ms | 18.34 ms |
| 200–300 | 9.43 | 10.53 | 11.6% | 29.00 ms | 19.19 ms |

繪製次數中位數分別為前版 796 / 793、新版 796 / 794。這是相近比賽進度
的樣本，沒有固定輸入重播或控制溫度，不能保證所有場景都有相同比例的提升。
新版截圖 `race.png` 確認角色、對手、賽道及 HUD 正常，仍未達到 30/60 FPS。
追蹤中不再出現兩個除錯監視器位址的慢速存取，仍可看到需要保護的時鐘讀取。

新版 15 秒 CPU 取樣共 11519 筆、沒有遺失；主要可見成本仍包括動態 TLS
解析 9.47%、hook 查詢 3.66%、函式進入檢查 2.69%。這些是 CPU 樣本占比，
不是整格時間占比；取樣跨過比賽起始，不直接與前一輪比例比較。

正式套件為 `out/android/FreeRidersRecompiled-adreno-performance2.apk`，
沿用 minSdkVersion 29，沒有取樣用的 profileable manifest。其 `libmain.so`
與實測取樣套件 SHA-256 相同：
`d5aa6ee79be059db122151fcb9596d21253af7ab3b828ddde91ab6a937f2639e`。
shader pack 仍為上述圖形修正版，雜湊相同。

正式套件已安裝到裝置。最後改用 `SFR_SKIP_MOVIES=0`，保留 `cores`，
讓第一段影片正常播放結束；本次啟動通過開場、標題及 Offline Mode 選單
（`no-skip.log`、`final-menu.png`）。這是暫時避開自動提早結束影片的設定，
不是對影片終止問題的程式修復，也尚未做大量重啟穩定性測試。
已移除 debug.env 的自動啟動、平行追蹤及影片等待 override，裝置停在選單。


## 2026-09-25：分開計量執行許可，避免 critical-section 等待取得全域許可

先前 `holders` 把全域與六個核心的時間加在同一個計數器，既會跨執行緒重疊，
同一執行緒持有全域與核心時也會重複計入。現在每個 `GuestExecution` 各有
計數，在 Present 取樣時連仍在持有中的區間一併結算，釋放時只計後續區間。
`holders` 現在只表示全域許可；`core0_holders` 到 `core5_holders` 各自獨立。
各欄仍只列前五名，不應把列出值當成所有執行緒的總和。

`main_blockers` 表示「guest 1 在此許可的 ready queue 裡」與某個 owner
持有它的重疊時間；`main_ready_unowned_ms` 是主執行緒已排隊、卻暫無 owner
的時間。`main_ready_ms` 是兩者完整總和。這些時間在當前 snapshot 截斷，
有別於原本在等待完成後才整段入帳的 `main_queued_ms`。全域持有時間是
牆鐘時間，也可能包含 owner 等待核心或被 OS 換出的時間，不等同 CPU 執行時間。

`GUEST_HOST_THREAD` 印出 guest ID、Linux TID（Windows thread ID）及 worker
入口，可用 Simpleperf 按 TID 分析；不必在每次客體函式進入新增紀錄。

`Lease::run_wait` 對 attached 執行緒沿用 `run_blocking`；對 detached
執行緒只讓出並重新取得 companion core。critical-section 的 pending wait
使用此 helper，完成交接仍由 `GuestCriticalSections` 的 mutex 保護。
`SFR_CRITICAL_WAIT_GLOBAL=0` 可選用不先 attach 的實驗路徑，`=1` 使用原本路徑，
可用同一二進位做對照。最終預設維持原本路徑；本輪未取得可靠效能改善證據。
沒有移除全域許可或任何記憶體保護。

驗證：Windows 的 guest_execution、guest_critical_sections、guest_memory、
pending_guest_write、guest_threads、guest_wait、async_completion_primitives
七項通過；新增測試包括 live snapshot、scope 隔離、主執行緒 ready 重疊歸因、
detached 等待時同核心 peer 能前進、取消，以及另一執行緒仍持有全域許可時
完成真實 critical-section 交接。Android 上排程與 critical-section 測試也通過。

注意：`draw_ms` 已包含 `record_ms`、常數讀取與紋理查詢，不能把這些分項
再次相加。round2 的 CAS 呼叫堆疊中，342 筆 `__aarch64_cas2_acq` 有 291 筆
位於 GuestMemory::check/read_scalar 的 shared_mutex 進出；它是 userspace
CPU 樣本占比，不能直接換算成每格可節省毫秒。

本輪資料：`out/android-performance-round3/`。`measure.py` 使用 101 個時間戳
計算 100 個間隔，分項統計使用後 100 筆（與間隔對應），並提供算術平均值。

初步取樣（candidate-initial.perf.data，15 秒、12937 筆、0 lost）按主執行緒
TID 16482 拆開後有 5165 筆。inclusive 呼叫樹中 sub_82809570 佔 60.43%，
sub_824F56E8 佔 28.23%；後者實際執行的是 native indexed-draw hook，不能把
原始重編譯函式的內容當成該樣本的工作。NativeRenderer::draw 佔 14.93%。
這些是相互包含的 CPU 樣本比例，不可相加，也不是牆鐘時間。
guest 29（TID 16531）有 3648 筆，主要沿 sub_827C9508 / sub_827D7250
等呼叫鏈執行；尚未確認該工作語意。原先全程序的 flat ranking 無法排除熱點。

第一趟新版比賽樣本曾出現 pipeline_ms 約 1011、657、227 ms 的單格卡頓；
它們已包含在 record_ms 與 draw_ms。這是管線建立的延遲，與平常十多 FPS
的持續成本需分別處理。該趟 100–200 / 200–300 為 14.97 / 12.12 FPS，
僅供定位，並非有效的同條件 A/B 結論。

舊等待政策的三次開場嘗試均停在約 21–25 秒（其中一次 skip movies=1）；
新政策一次成功進入比賽。尚未判定開場卡住的根因。測試增加
SFR_CRITICAL_WAIT_GLOBAL_AFTER_PRESENT=5000，讓兩趟都以新政策通過
開場，再讓 baseline 於第 5000 次 Present 後採用舊政策。
Present 訊息改成一次輸出完整字串，避免 detached worker 的 RESULT 訊息
插進數值欄位；增加 frame 欄位，量測時拒絕不連續的視窗。

後續新版開場也曾在切換點之前卡住（presents=652），因此開場問題不屬於
舊等待政策獨有。用 skip_movies=1 / movie_allowance_ms=1 成功取得 baseline
比賽，但新版同設定也有一次停在 presents=669；這個啟動穩定性問題仍未解決。
上述設定僅作這輪比較的開場 workaround，不能視為修復。

最終同版本對照的 baseline（permit-final.apk、舊政策自 Present 5000 生效）
成功取得連續比賽格；第一個 race frame 為 8001，兩視窗的實際 frame 是
8101–8201 / 8201–8301。結果：

| 視窗 | FPS | draws 中位數 | 全域 ready 平均 ms | 其中無 owner 平均 ms | pipeline 平均 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 100–200 | 14.76 | 796 | 18.84 | 6.27 | 0.00 |
| 200–300 | 11.05 | 799 | 17.97 | 6.20 | 22.02 |

第一視窗的主要列出 blocker 為 guest 18 約 2.94 ms、15 約 2.04 ms、
16 約 1.76 ms、26 約 1.51 ms、37 約 1.34 ms；各格只列前五名，這些平均值
是列出部分的平均，不是每個 guest 的完整累計。ready 無 owner 期間可能含
執行緒喚醒、排程與 companion 釋放交接，不能直接當成可刪除的等待。

同設定候選版三次重啟未通過開場（紀錄 candidate-final-start*-failed.log），
所以 **沒有完成同版本 A/B，不能用早期候選版 14.97 / 12.12 對上述 baseline
計算提升率**。早期候選與最終候選之間改過 Present 訊息格式與測試切換點，
開場設定也不同。後續兩者都能遇到開場停住，根因尚未判定。

最終交付保留新量測與 detached-wait 測試工具，但將 critical wait 預設恢復
原本行為；核心-only 路徑必須明確設 SFR_CRITICAL_WAIT_GLOBAL=0 才啟用。
測試用自動啟動、hang 與等待切換設定已從裝置移除，恢復 skip_movies=0、cores。
下一步應先穩定啟動／建立固定重播，再針對 native indexed draw 呼叫鏈與
首次 pipeline 建立分別優化；不能把本輪判讀修正報成 FPS 改善。

交付 APK：`out/android-performance-round3/permit-instrumented.apk`（已安裝）。
minSdk 29、無 profileable；libmain.so SHA-256：`6fee20c394a824b36fd805fcf67723c396c3cca5642547ee7d8d62ec8a76df68`。
shader pack 維持既有圖形修正版。最終僅改回等待政策預設值，未重做比賽效能
量測；Windows 排程／critical-section 回歸再次通過，裝置停在啟動器。


## 2026-09-25：開場卡住的完成通知／自我暫停競爭

`out/android-startup-round4/trace1.log` 捕捉到原本只靠 HANG_THREAD 看不到的
呼叫順序：guest 35（worker 824C39C8）先 SetEvent(0x72100200)，主執行緒
完成等待、ResumeThread(0x72200088)、再次等待相同完成事件，guest 35 才
NtSuspendThread(-2)。最後一次主執行緒呼叫鏈是 824C3A98 → 824A01EC。
這時 resume 發生在 suspend count 為零的時候，不會保留為未來的喚醒；
隨後自我暫停使主執行緒永遠等不到下一次完成。關鍵順序在 19976–19994 行。
這次不是 GPU 等待，也不是全域許可仍被其他執行緒占住。

修正只涵蓋原遊戲 LR=824C3A3C 的「通知完成→自我暫停」配對：在發送完成
事件之前先登記一份暫停，後續 self-suspend 消耗這份登記，不再累加一次。
早到的 resume 因此可以解除已登記的暫停。一般或其他執行緒發起的 suspend
保持原有計數語意，並非把所有 ResumeThread 改成可累積的喚醒通知。
`SFR_COMPLETION_SUSPEND_HANDOFF=0` 可關閉這個特定相容性修正作診斷比較。

同時，等待自行暫停的 callback 現在在釋放全域許可前取得穩定 record，等待
時只讀 atomic suspend count。原本 callback 在許可外遍歷 registry 並讀取
非 atomic 計數，會與其他執行緒建立／恢復發生資料競爭；保留原本 1 ms 輪詢
與取消行為，不在這輪另改等待機制。

新增回歸先在未登記暫停的實作上失敗（early resume consumes the announced
suspension），修正後通過；也涵蓋晚到的恢復、一般自我暫停、外部巢狀暫停、
重複登記、錯誤輸出位址與取消。Windows 六項相關回歸全部通過，Android
上的 guest_threads 測試也通過。

診斷使用 `SFR_THREAD_WAIT_TRACE=1`，只追主執行緒及 guest ID >=30 的部分
等待／事件／暫停 import；包含等待 handle 與主客體堆疊鏈，預設關閉。

套件 `out/android-startup-round4/completion-handoff.apk`：minSdk 29、無 profileable，
libmain.so SHA-256：`ff4ec895f3eb8713f3f4677bdb69b7172031a86bb952943b7b4ecc21271f9fe7`。Shader pack 與先前修正版相同。

實機驗證：修正後連續三次冷啟動都通過開場並到達標題畫面：
1. 正常播放、追蹤開啟（fixed-start1.log，後手動跳過剩餘影片）。
2. skip_movies=1 / allowance=1、追蹤關閉（fixed-start2.log）。
3. 正常播放、追蹤關閉（fixed-start3.log，後手動跳過剩餘影片並進入比賽）。
這是三次驗證結果，並非長時間或所有場景的穩定性保證。

第三趟成功進入同一場 Free Race，取得 906 個 draws>600 的格；紀錄沒有
HANG_REPORT 或 RUNTIME_STOP。100–200 / 200–300 視窗 frame 連續，分別為
14.68 / 11.40 FPS，draws 中位數 796 / 793。第二視窗 pipeline_ms 平均
21.68 ms，仍有建立管線的停頓。本輪是修復漏喚醒，不宣稱 FPS 顯著改善。
套件已安裝；裝置留在比賽暫停選單。debug.env 已回復 skip_movies=0、cores，
移除自動啟動、hang、追蹤與所有這輪比較開關。

後續具體目標：Plume 的 vkCreateGraphicsPipelines 目前傳入 VK_NULL_HANDLE
作為 pipeline cache（plume_vulkan.cpp:1638）；可先量測共用／持久化 cache
是否減少反覆建立管線的成本，再處理持續每格的 indexed-draw CPU 成本。
這部分本輪沒有修改，不能把 cache 的可能收益當成已實現的改善。

## 2026-09-25：持久化 Vulkan pipeline cache，消除重複編譯的大停頓

沿用 round4 的啟動漏喚醒修正，這輪只處理管線快取。原來 Plume 的 graphics /
compute pipeline creation 都傳入 VK_NULL_HANDLE；現在可共用一個 device-owned
VkPipelineCache。NativeGraphics 在開始繪製前載入它，NativePipelineCache 的
背景執行緒每 10 秒檢查建立次數，有新增才匯出；正常結束也會保存，並先停止／
join 背景執行緒，再銷毀裝置。Android 強制結束時可保留最近完成的快照。

預設位置是工作目錄下 `pipeline-cache/vulkan.bin`（Android 即 app files 下），
`SFR_PIPELINE_CACHE=0` 關閉整條快取路徑；`SFR_PIPELINE_CACHE_PATH` 可指定測試檔。
檔案限制 64 MiB，外層有格式版本、長度及完整性 checksum；送給驅動前檢查 Vulkan
version-one header 的 vendor / device / UUID。損壞、不相容或無法讀取會用空快取；
驅動拒絕初始資料時再試空快取，建立快取失敗則繼續原本路徑。新檔先寫入旁邊，
再原子替換舊檔；不先刪除最後一份成功快取。

使用 flags=0 的 Vulkan 內部同步，讓建立管線與背景匯出可並行；size query 與
copy 之間快取可能變大，因此 VK_INCOMPLETE 最多重試三次，保留舊快照等待下次。
這遵循 [vkCreatePipelineCache](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreatePipelineCache.html)
與 [vkGetPipelineCacheData](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html)
的同步與資料取得規則。Plume 的修改存為 `patches/plume-pipeline-cache.patch`，
列在 dependency lock 中，bootstrap verify-only 已驗證可重現。

四趟使用**同一份 APK**、同一個 Free Race / Sonic / 初始賽道與裝備選擇，比賽開始後
沒有輸入。沿用 round3 測量腳本的 draws>600 篩選與 100 個 frame intervals；
下表兩個視窗的 frame 都連續，draws 中位數全部是 796 / 793。

| 模式（依執行順序） | 100–200 FPS | 200–300 FPS | 第二段建立數 | 第二段 pipeline 累計 ms | 第二段最慢一格 ms | 電池 °C 起／迄 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 關閉快取 A1 | 14.65 | 11.43 | 24 | 2022.84 | 1189 | 40 / 39 |
| 首次空快取 B1 | 14.72 | 13.71 | 29 | 616.46 | 344 | 39 / 38 |
| 載入快取 B2 | 15.45 | 14.23 | 22 | 9.41 | 160 | 38 / 38 |
| 再次關閉 A2 | 15.30 | 11.38 | 27 | 2077.92 | 1233 | 38 / 38 |

B2 的初始化實際載入 2,612,004 bytes；A1 / A2 均沒有建立快取物件。B2 第二段
沒有超過 200 ms 的 interval，A1 有 4 個、A2 有 3 個。A2 在 B2 之後執行，
同樣約 38°C，卻恢復兩秒以上的管線建立成本與一秒以上的最慢格，支持改善來自
快取，而非只因為執行順序／溫度降低。B1 在同次執行中也能重用驅動資料，所以
尚未有磁碟快取時就比 A1 少一些編譯成本。

限制：沒有固定輸入重播，也沒有控制 SoC 溫度／頻率。繪製次數中位數一致仍不
代表逐格相同，第二段建立數 24 / 29 / 22 / 27 也直接顯示這一點。此結果足以
支持明顯縮短編譯停頓；不把前一段不到 1 FPS 的差異當作另一項 CPU 優化成果。
暖快取也不代表所有新賽道／新狀態都已經快取，首次遇到仍可能需要編譯。

計時仍不能相加重複計算：pipeline_ms 包含在 record_ms 與 draw_ms 內。B2 第二段
平均 pipeline_ms 0.094、record_ms 4.917、draw_ms 11.655，main_ready_ms 17.497。
整體仍約 14–15 FPS，沒有達成 30 FPS。後續應在暖快取場景量每格固定 CPU 成本，
包括 indexed-draw hook 與客體執行／排隊，不能繼續把編譯停頓當成全部效能問題。
背景保存的 wall time 在 B2 約 5.7–15.8 ms／次，並非每格的主執行緒成本；
建立數不變時不會持續重寫檔案。

驗證：
- 先用未實作的 file store 跑回歸，於 first cache save succeeds 失敗；實作後
  Windows 與 Android arm64 通過。涵蓋 byte-exact round trip、覆寫、每個截斷點、
  header / payload 位元損壞、超過大小限制、Vulkan header 不相容及替換失敗不破壞原檔。
- 五項 host CTest 通過：pipeline_cache_file、native_graphics、guest_execution、
  guest_critical_sections、guest_threads。NativeGraphics 實際 GPU copy 另以 Vulkan
  執行通過；Vulkan native_presentation 的 clear / readback 行為測試通過。
- 四次比賽分別取得 537 / 425 / 510 / 523 個 draws>600 的格，紀錄沒有 HANG_REPORT
  或 RUNTIME_STOP。關閉與暖快取的截圖皆能正常辨識角色與場景，未做逐像素比較。

所有原始紀錄、截圖、快取檔與測量摘要位於 `out/android-pipeline-round5/`；
`comparison.json` 可由同目錄的 compare.py 重建。APK 是 `pipeline-cache.apk`，
minSdk 29、targetSdk 35、arm64-v8a、無 profileable。libmain.so SHA-256：
`29db52f0f21b3b69008e4c18fa5a827f7ed2db423f618d69e83fca0564ef10bd`；
shader pack 保持 `d430c0c0bff5f7b9937a16d67dd3b5059bc9165296a440a73b32a9201fd93b4a`。

交付：新版已安裝，暖快取複製到預設 `pipeline-cache/vulkan.bin`，以正常開場設定
重新啟動並確認載入 2,631,092 bytes，之後到達標題畫面。`debug.env` 已逐 byte 回復
原檔（skip_movies=0、cores），移除自動啟動與本輪 A/B 開關。最後狀態與紀錄是
`delivery.png` / `delivery.log`；這一輪沒有提交 Git commit。


## 2026-09-25：等待結果日誌 A/B 與暖快取 CPU 取樣

round5 暖快取紀錄的 200 格中有 4,771 行，其中 3,852 行 CriticalSectionWait、
200 行 NtWaitForMultipleObjectsEx 成功結果沒有遵守 SFR_TRACE_IMPORTS=0。
這輪讓這兩處繼承 trace_imports；NtWaitForMultipleObjectsEx 的高位錯誤狀態仍印出。
新增 SFR_TRACE_WAIT_RESULTS=1/0，僅切換這兩處，方便同一 APK 比較；未修改等待、
排程、許可或記憶體檢查。日常設定 TRACE_IMPORTS=0 時預設安靜，未設定的稽核模式仍有日誌。

測試使用同一份 profileable APK、既有暖快取、同一 Free Race / Sonic / 初始賽道與
裝備，開始後不輸入，順序開／關／關／開。沿用 draws>600 的 100–200 / 200–300
視窗，每段 100 個 interval。兩段實際 frame 編號皆連續，draws 中位數均為 796 / 793。

| 日誌（依執行順序） | 100–200 FPS | 200–300 FPS | 200 格總行數 | 其中等待結果 | 電池 °C 起／迄 |
| --- | ---: | ---: | ---: | ---: | ---: |
| on-a | 14.21 | 14.59 | 5340 | 4578 | 38 / 37 |
| off-a | 14.06 | 14.41 | 733 | 0 | 37 / 37 |
| off-b | 15.51 | 14.53 | 701 | 0 | 37 / 37 |
| on-b | 14.29 | 14.05 | 5435 | 4709 | 37 / 37 |

關閉後量測區間的日誌少約 87%，兩種等待結果確實歸零；四趟取得 467 / 461 /
460 / 478 個比賽格，沒有 HANG_REPORT / RUNTIME_STOP。但 FPS 未呈現可重複的提升：
off-a 略低於 on-a，off-b 前段較快，後段差距小。這輪僅確認成功抑制日誌，
不把單次 15.51 FPS 當成優化收益，正常速度仍約 14–15 FPS。

on-a 的第二段另有一格新管線建立花 248.278 ms；該段平均 pipeline_ms 為 2.561，
其餘三趟是 0.110 / 0.082 / 0.097。首段四趟都是 0。這是快取補齊的混雜因素，
不能算成日誌改善。比賽沒有固定輸入重播、未控制 SoC 溫度／頻率，電池溫度與
相同 draws 中位數只能改善可比性，不能保證逐格相同或證明微小差異不存在。


在 off-a 測量結束、暫停後再恢復比賽，另錄 15.0167 秒 task-clock:u、500 Hz
含呼叫堆疊的 simpleperf：12,526 筆、零遺失。這是較後方場景，並非上述測量視窗。
由 GUEST_HOST_THREAD 對應 guest id 與 Android TID，不必加入每次函式進入的計數器：

- 主客體 1（TID 18258）：4,403 筆，占全程序 35.15%。native_draw 包含子呼叫占
  該執行緒 26.96%；indexed-draw hook sub_824F56E8 25.39%；NativeRenderer::draw
  14.01%。三者有包含關係，不能相加。自身成本有 TLS resolver 5.61%、native_draw
  3.07%、memmove 2.61%、decode_indices 2.07%、byte-vector insertion 1.70%。
- 客體 29（TID 18307）：3,614 筆，占全程序 28.85%。
  827C9508 → 827D7250 → 827D66C0 → 827D6088 這串呼叫包含其 95.52% 的 CPU 樣本；
  sub_827DA100 包含子呼叫占 59.27%，TLS resolver 自身 12.51%。尚未判定這些
  數字命名函式的遊戲語意；不能因最大 leaf 小，就推論沒有可定位的熱點。
- 全程序自身成本：TLS resolver 7.51%，16-bit CAS 5.26%。這是目前版本的觀察，
  沒有與舊版相同場景重測，不能當成 TLS 修改前後的提升／退步。

原始堆疊中，659 筆 CAS 有 603 筆（91.50%）同時經過 GuestMemory 與 libc++
shared_mutex，有 491 筆（74.51%）經過 check_reservation_context；集合互相重疊。
代表路徑是 pthread_mutex_lock → shared_mutex 的 lock_shared / unlock_shared →
GuestMemory::check / read_scalar。來源是函式庫內部鎖，無須在應用程式找到
atomic<uint16_t> 的 compare_exchange。主客體只有 2 筆 CAS，客體 29 只有 14 筆；
最多的是客體 14（236）、39（132）、36（95），所以這個全程序百分比並不是可直接
從主執行緒扣掉的時間，也尚未證明這些工作在每格的關鍵等待路徑上。

程式碼可解釋反覆檢查的原因：check_reservation_context 讀 PCR+0x100 與
thread_object+0x14c；GuestThreads::initialize 只 commit 0x2D8 / 0xAB0 bytes，
而 fast-page 只承認整頁已 commit，因此這些合法位址仍走慢路徑。detached guest
的 check 與 read_scalar 查 provider 時會分別取 layout reader lock。後續可研究
減少此類重複驗證，但直接擴大 commit 會改變非法位址的偵測範圍，移除鎖則必須證明
layout/provider 的生命週期；這輪均未採用。

主執行緒的具體後續方向是 native_draw 每次重建頂點宣告，以及 NativeRenderer::draw
每次逐欄位組 pipeline key 的成本。先保留完整狀態與 guest 寫入時的失效條件，再比較
減少重複解析／複製的方案，不能僅以宣告指標相同就假定內容未變，也尚未宣稱 FPS 收益。
CPU 樣本不含阻塞時間，這些比例都不是每格 wall time 或可直接相加的加速上限。

原始資料位於 out/android-logging-round6/：四趟 log / measure.json / 截圖、
comparison.json、quiet.perf.data、main-report.txt、worker29-report.txt，以及
profile_summary.py / profile-summary.json 的逐筆堆疊歸因。unstripped 符號檔的
build ID 是 f8c5b577dce2e847d1b9020c090833ecc640d308。profileable 與正常 APK
內的 libmain.so / shaders.pack 完全相同，雜湊記在 build-identity.json：
libmain.so a97a6d085df8ddf424d4886d6b31f790a456f74bd1238c443febf25ed99deb68，
shader pack 仍是 d430c0c0bff5f7b9937a16d67dd3b5059bc9165296a440a73b32a9201fd93b4a。

驗證與交付：Android 重新編譯成功，最終 build 檢查為 no work to do；四趟比賽皆
成功完成取樣。這是兩個低風險日誌條件，未新增只重述實作的單元測試；實機開／關
輸出計數確認開關有效，錯誤狀態保留條件經程式碼檢查，未刻意在實機注入 NT 錯誤。
正常 wait-logging.apk 已安裝，manifest 確認無 profileable，原本的 debug.env
逐 byte 還原（skip_movies=0、cores），保留暖快取並在正常開場確認載入 2,781,269
bytes。delivery.png / delivery.log 記錄恢復後的開場動畫與啟動狀態。沒有提交 commit。


## 2026-09-25：一次填入 pipeline key，保留逐 byte 相同的查詢資料

接續 round6，先處理 CPU 取樣中 byte-vector insertion 占主執行緒 1.70% 的
具體路徑。原本 NativeRenderer::draw 對每個欄位呼叫 vector::insert，20 個 input
元素會超過 100 次；現在先算完整長度、resize 一次，再以固定大小 memcpy 填入。
新舊 serializer 在 native_pipeline_key.h；map lookup、pipeline creation、常數／
頂點上傳與 command recording 的程式碼保持相同。沒有快取前一次 NativeDraw，
也沒有把 guest declaration 指標相同當成內容相同。

保留舊 key 的欄位順序、寬度、semanticName 指標識別、blend 物件 padding、
stencil 開關與條件欄位；沒有順便更改 pipeline identity。SFR_PIPELINE_KEY_BULK=0
可切回原方式，預設使用新方式；SFR_PIPELINE_KEY_VERIFY=1 同時計算並逐 byte
比較兩者，不一致即停止。實機計時時驗證開關為 0。

回歸先以空實作確認 byte identity 測試失敗，再實作通過。測試涵蓋空／1／20／36
個元素、重用 buffer 時縮小與增長、stencil 開關、depth/blend/spec mask、shader
與各 input 欄位變動、不同 blend padding，以及每 draw 常數／count 不影響 key。
Windows 重編後的 native_pipeline_key、guest_graphics、guest_render_state、
guest_blend_request 四項通過；Android arm64 也通過。Android 測試第一次直接
執行缺 libc++_shared.so，補上套件內的 runtime 並設 LD_LIBRARY_PATH 後才成功，
該次 loader failure 不列為測試通過。

手機局部 microbenchmark：50 萬個 20-element、stencil-enabled key，順序舊／新／
新／舊，耗時 339.737 / 11.513 / 11.535 / 312.595 ms。它只是 serializer 自身的
診斷量測，不能當成 FPS 加速倍數；按 800 draw 粗估是每格約 0.5 ms 的尺度，
也不是已量到的 frame-time 差值。實際遊戲另跑雙路比對，包含 456 個比賽格，
累計超過 140 萬次 key 一致，無 mismatch、RUNTIME_STOP、HANG_REPORT。
verify-race.png 可辨識角色與場景，未做像素相同的 replay 比較；驗證趟不計入 A/B。

四趟計時都使用同一份正常、無 profileable APK，同一 Free Race / Sonic / 初始
賽道及裝備，開始後無輸入，原／新／新／原順序。每次載入同一個 2,781,269-byte
暖快取；電池溫度起迄均 36°C，所有視窗 frame 連續，實際開關均符合預期。

| 模式 | 100–200 FPS | 200–300 FPS | draws 中位數 | draw_ms 平均（兩段） | record_ms 平均（兩段） |
| --- | ---: | ---: | --- | --- | --- |
| legacy-a | 14.83 | 14.51 | 796 / 792 | 9.902 / 11.749 | 4.818 / 4.998 |
| bulk-a | 15.83 | 14.65 | 796 / 792.5 | 8.844 / 10.206 | 4.010 / 4.205 |
| bulk-b | 16.32 | 15.22 | 796 / 793 | 8.370 / 9.900 | 3.837 / 4.268 |
| legacy-b | 15.60 | 13.86 | 796 / 793 | 9.185 / 10.912 | 4.440 / 4.893 |

各模式合併 400 個 interval，FPS = 400 / 總秒數：原方式 14.67、新方式 15.48，
這組樣本約增加 5.5%。兩趟新版的兩段 FPS 都高於對應的兩趟原方式，draw_ms 也較低；
切回原方式後 draw_ms 回升。平均 draw_ms 原 10.437、新 9.330，差約 1.107 ms。
這支持保留一次填入的修改，但只有每模式兩趟，沒有固定 gameplay replay、SoC
頻率鎖定或 SoC 溫控；draws 第二段 792 / 792.5 / 793 / 793 也顯示不是逐格一致。
不能把 5.5% 當成所有賽道的保證，也沒有達成 30 FPS。

record_ms 包含在 draw_ms 內，不能再加一次；新舊 record_ms 也有差異，而 key
建立在呼叫 record() 之前，表示排程／量測波動等因素亦有影響，不把所有 wall-time
差值都視為 serializer 的直接 CPU 節省。首段 pipeline_ms 均為 0；第二段依序
0.092 / 0.054 / 0.102 / 0.067 ms，沒有前幾輪數百毫秒的新管線編譯混雜。
四趟各取得 436 / 442 / 440 / 425 個比賽格，均無 RUNTIME_STOP / HANG_REPORT。

原始紀錄、截圖、比較程式與結果在 out/android-pipeline-key-round7/。
套件 pipeline-key.apk 的 libmain.so SHA-256 是
3146937aa0ae158293975a19a87834e18169c08a60a1a57ece4e53c1e9465c17；shader pack
保持 d430c0c0bff5f7b9937a16d67dd3b5059bc9165296a440a73b32a9201fd93b4a。

交付：上述同一 APK 已留在手機上，原 debug.env 已逐 byte 還原（skip_movies=0、
cores）；正常啟動紀錄確認 bulk=1、verify=0，暖快取載入 2,781,269 bytes，
畫面已回到標題頁。delivery.log / delivery.png 保存最後驗證。沒有提交 commit。

## 2026-09-28：客體處理器改排到不同的實體核心（Windows）

比賽的瓶頸在客體主執行緒（`holders=1:` 約 36 ms 一格，算繪路徑約 8 ms），所以速度幾乎
取決於 CPU 單核與排程。客體的 6 個處理器原本依序對到 Windows 的邏輯處理器 0–5；有
超執行緒的 CPU 上 0/1、2/3、4/5 是同一顆實體核心，六個客體處理器等於擠在三顆核心上
（實測 i7-6850K：比賽 22–30 fps）。

現在依 `GetLogicalProcessorInformationEx(RelationProcessorCore)` 排序：先取每顆實體核心的
第一個硬體執行緒（效率等級高的核心，也就是混合架構的 P 核，排前面），用完才輪到各核心的
第二個。啟動時印出一次 `NATIVE_HOST_PROCESSORS group= order=`。
`SFR_HOST_PROCESSORS=sequential` 還原舊的順序，方便 A/B 比較。

## 2026-10-01: Bounded vertex-declaration cache on AYN Thor

`native_draw` used to rebuild vertex input locations, DEC3N offsets and component-swap
masks on every draw. `NativeVertexLayoutCache` keeps 16 decoded layouts per thread.
A hit requires the same declaration identity, stride, length and every declaration
byte. It takes one bounded snapshot for comparison, decoding and the stored key,
so a rewrite cannot associate an old decoded layout with a newer cache key.
Addresses alone are never sufficient. Guest ranges rejected by `fast_read` retain
the original scalar parser and its memory-check/exception order.

`SFR_VERTEX_LAYOUT_CACHE=0` selects the old parser. `SFR_VERTEX_LAYOUT_VERIFY=1`
compares cached and original results (all input fields, DEC3N offsets and swap masks),
throwing on a mismatch. Both are diagnostic environment controls, not launcher settings.
The cache does not change shaders, rendering resolution, game time or frame limits.

Validation:

- Host `native_vertex_layout`, `native_formats` and `native_pipeline_key` tests pass.
  Layout tests cover mutation at the same address, stride/count changes, eviction,
  component-swap invalidation, maximum declarations, failed decodes and invalid input.
- Android arm64 tests pass. Thor reference verification exceeded 3 million draws,
  including a Dolphin Resort race; Windows D3D12 and Vulkan reference runs exceeded
  6.1 and 5.8 million draws respectively, also into races, with no mismatch.
- The snapshot-only review fix followed the Thor reference run and preceded the
  final Windows reference runs and Android performance measurements.
- Android screenshots show the race clock advancing 12.38 to 38.97 seconds while
  file timestamps advance 26.568 seconds. Timing remains real-time.

Measurements use the same profileable APK, saved race fixture, warm pipelines,
720p/100% rendering and normal game timing. Whole 120-frame summary intervals
inside race-clock seconds 10–45 are selected using screenshot HUD anchors. Each
row includes 720 frames; the game is not a deterministic replay.

| Run | FPS | Draws/frame | Main queued ms/frame | GPU wait ms/frame |
| --- | ---: | ---: | ---: | ---: |
| Cache on | 23.07 | 827.16 | 6.89 | 0.89 |
| Cache off | 23.59 | 784.43 | 7.76 | 1.29 |
| Cache on, repeat | 23.43 | 791.42 | 8.02 | 0.99 |

These runs do **not** establish an FPS improvement or regression: rendering work
also differs by about 5%, race trajectories diverge and SoC clocks are not locked.
Battery temperature was 36–37 C for on and 37 C for off. Main-thread CPU leaf samples
for `native_draw` plus the layout/parser helpers total about 3.16% with cache and
3.96% without, but their sampling windows start at different race times. This is
supporting evidence of reduced parsing work, not a frame-time saving or an FPS claim.
The repeated cache-on run is within 0.7% of cache-off, with about 0.9% more draws.
No repeatable FPS gain is demonstrated. The cache is retained in the local test
branch for further evaluation; this work has not achieved 30 FPS.

Remaining sampled work includes generated guest functions, dynamic TLS resolution
and guest memory helpers. A private local-function-link binary was prepared but
not installed or adopted: earlier experiments had an unexplained loading failure
and no repeatable speedup. No new linker, affinity or global power policy is shipped.

Raw logs, profiles, matching native symbols, screenshots and runners are retained
locally under `out/thor-race-perf/`, including `layout-measure`, `layout-off`,
`layout-verify`, the two Windows verifier directories and `phase_compare.py`.
The normal test APK uses the exact measured `libmain.so` and shader pack, with the
shell profiling permission removed. It is a local test, not a published release.


## 2026-10-01: Jump-rating UI timing on AYN Thor

The race clock was already real-time, but the jump-rating UI advanced one
60 Hz timeline frame per rendered update. The UI task runs on guest thread 16,
while the race clock runs on guest 1. A first thread-local implementation had
no runtime effect; a metadata probe identified the thread mismatch.

The completed race delta is now published through an atomic scalar. Known UI
wrappers snapshot it once, replay bounded steps of at most one authored frame,
and evaluate intermediate actions instead of skipping them. Playback speed is
restored afterwards, including exceptions. Completion stops catch-up so the
owner can process it before a new cycle. Unknown timeline classes and explicit
reverse or greater-than-one playback rates retain their original path.
Inactive/unsupported race clocks publish a normal one-frame step.
`SFR_REALTIME_UI=0` retains original UI behavior for diagnosis. Desktop defaults
remain unchanged because real-time race timing is Android-only by default.

The child probe observes the actual rating timeline reaching frame 79, where
it deliberately pauses pending a later game event. Measuring `rank_in` through
`rank_roll` includes that wait, so it is not a pure animation-duration metric.

| Probe | Presents to observed frame 79 | Approximate wall seconds |
| --- | --- | --- |
| Original child probe, three ratings | 79 / 79 / 82 | 3.41 / 3.34 / 2.79 |
| Shared race delta, three ratings | 37 / 34 / 42 | 1.58 / 1.44 / 1.53 |

These are sampled upper endpoints (child tracing every fourth present) with
wall time interpolated from 120-frame summaries, not precise event timestamps.
The 79 authored frames correspond to about 1.32 seconds at 60 Hz. The trace
confirms intermediate frame actions and fractional steps are processed; the
remaining measurement difference does not establish a residual timing bug.
Both runs complete the 14,400-present fixture. Screenshots retain the race HUD
and rider. This establishes faster UI progression, not an FPS gain.

Windows and Android builds pass, as do host and ARM64 race-clock tests covering
20/24/30/60/120 FPS, half-speed playback, intermediate events, completion,
invalid deltas and bounded catch-up. Normal APK packaging excludes private
rank tracing and shell profiling permission. Evidence is retained locally in
`out/thor-race-perf/rank-*-probe*` and `rank-frame79-analysis.json`.

Loading stutters remain a separate issue. A baseline probe also encountered
an intermittent worker memory fault previously recorded before this UI change
(guest 46, last function 82918418, caller 822A4C38); repeating the same binary
completed. Neither the source of that fault nor loading performance is claimed
fixed here.

The final loop probe (`rank-step-probe`) also completes 14,400 presents. For
one rating, 35 wrapper invocations apply 80.316 authored frames through the
frame-79 hold, with no early completion return. Exact `rank_in` to `rank_roll`
timestamps across four ratings are 1.808/1.665/1.623/1.779 seconds, including
the game's deliberate hold. This supports preserving that hold.

The normal `0.4.5-perf-thor-ui-test` APK is installed on Thor with no debug.env.
Its SHA-256 is `2e1b2185e19b4237eba2eafb3518731b1b7dd0060a7dd6d079dc7831075fe2b0`.
The user's saves/settings were not cleared; fixture runs use benchmark-save.


## 2026-10-01: Physical allocation search on AYN Thor

Loading guest thread 12 spends sampled CPU time in physical allocation and
reservation queries. The prior allocator restarted a binary conflict search
from the arena's top for every allocation, calling both `available` and
`lowest_conflict` for each occupied candidate. A private probe records up to
3,551 candidates per request. The 3,391 logged requests total 3,780,956 probes
and 464 ms before mapping; that last measure includes pre-query bookkeeping,
and the logger filters out requests with at most 100 probes and at most 0.5 ms.
It is not a complete allocation census or pure search timing.

`GuestMemory::find_available_top_down` now walks the sorted reservation vector
backwards under the existing layout-read contract. It returns the highest free
host-page-aligned range, with partial reservation tails still occupying full
host pages. PhysicalMemory retains its original bounds, retained-memory reuse,
budget check, protection, mapping and publication order. No free-list cache,
new reservation state, guest timing or rendering change is introduced.

Validation:

- Regression tests compare the query to exhaustive page search across bounded
  windows, partial tails, holes, release, address zero, invalid ranges and the
  exclusive 4 GiB limit. Host guest/physical/virtual memory, memory-statistics
  and pending-write tests pass; guest and physical memory tests pass on Thor.
- Full Android and Windows production targets build. Source review found no
  actionable issue and independently compared 200,000 randomized old/new
  searches, including multi-page partial reservations and arbitrary windows.
- A private Thor verifier executes both searches against the same live map,
  checks exact optional address equality before mapping, and completes the
  14,400-present fixture: **3,569 queries, zero mismatch**. Old queries total
  **413.920 ms**, new queries **46.352 ms** (88.8% lower query time). New runs
  first and old immediately afterwards; timing includes host scheduling/cache
  effects. This is a query-level comparison, not an 88.8% Loading/FPS gain.
- A separate ARM64 microbenchmark with 2,500 dense reservations and 2,000
  repetitions measures roughly 235 ms -> 17 ms for 4 KiB searches. Conversely,
  1 MiB requests are about 2.8 ms -> 17 ms: the old algorithm can skip many
  small reservations per jump. This tradeoff is retained because the measured
  live query mix has a clear aggregate saving. An immediately free small
  range changes only by tens of nanoseconds per query in that microbenchmark.

Artifacts: `out/thor-race-perf/alloc-probe*`, `alloc-verify*`,
`allocation-bench.txt`, `allocation-query-comparison.json`, matching symbols
and fixture screenshots. The normal APK excludes ALLOC_PROBE/ALLOC_VERIFY.
Large Loading stutters and sustained race FPS remain separate bottlenecks;
this change does not establish an end-to-end loading-time or FPS improvement.


The normal non-profileable APK also completed the 14,400-present fixture with
no memory stop; the saved frame-12000 screenshot shows Dolphin Resort with
HUD race time 1:07.61. Scene/frame alignment differs from the diagnostic run,
so their summary FPS must not be directly compared. The normal run still has
a 222.985 ms maximum frame after present 9000: large stutters are not solved.
`alloc-normal.log`, its screenshots and `allocation-delivery.json` retain this
evidence. Installed version is `0.4.5-perf-thor-allocation-test`, SHA-256
`1f73fcc60939501534789abc695f9d47e72a6d36efd1f8efe848f36e4a02a5c6`.
The diagnostic override is removed; the user's save/settings were not cleared.

## 2026-10-01: Texture conversion on AYN Thor

The ARM64 compiler emitted scalar byte operations for texture endian modes 1
and 3, and variable-size `memcpy` calls for each untiled block. The conversion
now uses 16-byte NEON/SSSE3 permutations with scalar tails, and specializes
untile copies for the supported 4/8/16-byte blocks. Generic fallback behavior,
zero filling of unavailable blocks, GPU submission order and resource
lifetimes remain unchanged.

Host and Thor ARM64 native-format tests cover endian modes, unaligned spans,
incomplete groups, output guards, tiled row boundaries, padded pitches and
truncated source data. Windows and Android production builds pass. An
independent source review found no actionable findings.

A private verifier runs original and optimized conversion on every actual
uploaded texture, comparing all output bytes before upload. Its complete
14,400-present fixture includes Loading and Dolphin Resort racing:

| Measurement | Original | Optimized |
| --- | ---: | ---: |
| Endian conversion, total | 31.929 ms | 5.800 ms |
| Untiling, total | 74.037 ms | 44.082 ms |

All **999 uploads (159,179,776 source bytes) match exactly**. There are 777
tiled uploads; endian modes 1/0/2 occur 854/95/50 times. Mode 3 is covered by
unit tests and the standalone ARM64 benchmark rather than this live scene.
The optimized function runs first, then the original on a separate copy;
these timings include host scheduling/cache effects and are not an FPS or
end-to-end Loading speed comparison. Standalone paired benchmarks likewise
reduce endian 1/3 time and untile time for each supported block size.

A separate phase probe records 1,013 uploads, with 134.2 ms total fence wait
and 8.9 ms staging allocation time. The three longest waits are 33.9, 18.0
and 12.1 ms, at the fifth upload in their frames. The existing four-command-list
ring therefore remains a source of synchronous waits during upload bursts;
this CPU conversion change does not eliminate those waits. A cache observer
finds one dirty texture, whose bytes really changed, and zero unchanged dirty
textures in the verifier fixture. There is no measured justification here
for adding a content hash to every dirty-cache hit.

Local evidence: `out/thor-race-perf/texture-detail-analysis.json`,
`texture-verification-analysis.json`, `texture-bench-before.txt`,
`texture-bench-after.txt`, corresponding logs, private binaries and symbols.
The normal APK excludes the private phase timers, duplicate conversions and
dirty-texture observer.

The normal APK also completes the 14,400-present fixture. Its frame-14400
screenshot shows Dolphin Resort racing at HUD time 00:57.27, with rider,
track and HUD rendered. Frame metrics remain enabled by the fixture, but the
normal binary has no private per-upload verifier. The maximum frame after
present 9000 is still 181.634 ms (62.361 ms in the overall texture path);
other long frames have little texture work. Neither all stutters nor sustained
race FPS is claimed fixed. Scene alignment differs from previous captures,
so their maximum frames are not an equivalent-work performance comparison.

`texture-normal.log` and `texture-normal-analysis.json` retain this evidence.
Installed version is `0.4.5-perf-thor-texture-test`, SHA-256
`ad81403c17f7e8253f50bfa562357e827e9162d1afe9946d2deef53b3ea02ccc`.
The fixture's debug.env is removed and the launcher restored. User saves and
settings were not cleared; the scripted run uses the separate benchmark save.

## 2026-10-01: Batched texture upload experiment

`NativeUploadBatch` accumulates texture copies in two command-list/fence
slots instead of submitting once per texture. `NativePresentation` drains
pending copies on the same graphics queue before each consuming submission,
including explicit flush/readback and an otherwise empty flush. Complete
flushes wait upload fences before descriptor recycling. Asynchronous presents
retain submitted staging until its copy fence completes on slot reuse.

Retained staging has a 32 MiB budget across both slots. Reaching it submits
and finishes copies without flushing the partially recorded graphics list or
resetting the vertex ring. One oversized staging buffer is allowed and is
immediately submitted and waited. The incoming allocation exists before the
helper owns it, so the retained-byte counter is not a total-process-memory
cap; GPU textures, allocator overhead and the incoming buffer are additional.
The upload path retains destination textures before recording, so failure in
later metadata work cannot leave teardown submitting a dangling destination.

`SFR_TEXTURE_UPLOAD_BATCH=0` selects the old per-texture submission path.
`SFR_TEXTURE_UPLOAD_WAIT=1` also retains its existing synchronous legacy
behavior. Normal default enables batching. Frame diagnostics add
`upload_submissions` and `upload_wait_ms` for batched work since the prior
sample, and cumulative `upload_peak_bytes` / `upload_budget_drains`. Zero
values in legacy mode do not mean its uploads perform no submissions/waits.

Validation so far:

- Real GPU helper tests on Vulkan and D3D12 verify 54 immutable buffer copies
  across six submissions, repeated slot reuse, bounded-budget and oversized
  drains, empty finish and destructor behavior.
- Renderer texture readback tests on both backends verify 16 successive
  versions of the same guest range, drawn into separate columns, across five
  frames including asynchronous presents. Each column retains the intended
  version. Legacy and batch paths produce identical expected pixels; the
  batch path submits once per graphics batch, and an empty graphics flush
  drains a pending texture upload.
- Presentation tests verify upload callbacks precede completion callbacks
  for empty/recorded flushes and run during present. Callback removal is
  checked. Windows and Android production builds pass.
- Independent source review found an exception-lifetime issue; destination
  retention was moved before command recording. Re-review found no remaining
  concrete correctness findings in integration or helper lifetimes.

Thor A/B/A captures use one APK and one isolated benchmark save, with only
the batching switch changed. Results must account for different scene
progression; fixed present indices alone do not establish equivalent work.

All three 14,400-present runs completed, in off/on/off order:

| Metric | Off A | Batch on | Off B |
| --- | ---: | ---: | ---: |
| Textures uploaded | 993 | 1,005 | 1,013 |
| Texture work in frames with uploads (ms) | 501.614 | 283.855 | 461.304 |
| P95 frame ms after present 9000 | 52.729 | 52.926 | 53.159 |
| P99 frame ms after present 9000 | 67.723 | 66.998 | 69.183 |
| Maximum frame ms in that window | 162.182 | 150.052 | 231.927 |
| Frames over 100 ms in that window | 12 | 7 | 9 |

The on run groups 1,005 texture copies into **163 submissions**, compared
with one submission per texture in the legacy path. Batched upload fence
waits total **7.171 ms**. Peak retained staging is **33,397,760 bytes**
(31.851 MiB), with one budget drain. One 26-texture burst costs
36.918 / 6.228 / 37.135 ms in the texture path across off/on/off; its overall
frame durations are 77.061 / 40.882 / 76.765 ms. Draw counts differ slightly
(567/563/567), so this is supporting evidence rather than a controlled
identical-frame timing claim. A 237-texture burst in both Off A and Batch on
still costs about 65 ms in texture handling: that remaining cost is not
eliminated by fewer submissions.

These results support reduced submission overhead and upload spikes, not a
proven sustained-FPS increase. The aggregate P95 is effectively unchanged.
Loading and race transitions occur at different present indices; even upload
groupings vary (Off B has a 342-texture burst). The percentile/max table is
descriptive, not a matched-scene speedup estimate. Actual battery temperatures
are retained in the run metadata and are not GPU/CPU temperature measurements.

Evidence: `out/thor-race-perf/batch-off-a*`, `batch-on*`, `batch-off-b*` and
`upload-batch-comparison.json`. The on-run final screenshot shows Dolphin
Resort with rider, track, HUD and race clock 01:24.25. The fixture includes
frame metrics; it does not contain private duplicate-conversion instrumentation.

The installed normal APK is `0.4.5-perf-thor-batch-test`, SHA-256
`b036b5381a43388926b6085cb56c16014f25939d25abd03f479cd86c5de96326`.
After the last control capture, debug.env was removed and the app restarted
to its launcher; normal operation uses the batching default. User save and
settings remain intact. This is a local test package, not a published release.


## 2026-10-02: Thor surface resize and controller steering

The batched-upload test was playable, but the user reported a crash after rotating to portrait and back, and weak controller steering. These were investigated separately from throughput.

### Surface resize

- The user's crash and a fresh AYN Thor size-change reproduction both faulted in Adreno `vkQueueSubmit` (`resize-before.log`, `resize-before-logcat.txt`).
- A temporary validation-layer APK reported `VUID-VkSubmitInfo-pSignalSemaphores-parameter`: the signal semaphore was null. The renderer resized its swap-chain images without resizing its image-indexed synchronization arrays.
- Plume also reused the last *returned* image count as the next `minImageCount`. The driver can provide more than requested; during six size changes the intermediate fix reached 18 images. The pinned patch now preserves the original request, applies current surface min/max limits on each resize, and accepts a smaller actual count when it still satisfies the new request.
- The application flushes rendering, waits on the presentation queue under its queue mutex, allocates fresh synchronization for the actual image count, checks native semaphore handles, and rebuilds blit targets. An explicit acquired-index check prevents out-of-range access.
- The final validation run (`resize-final-validation2`, PID 18376) survived six commanded size changes (16 logged swap-chain rebuilds) with image count fixed at **4**. Its logcat confirms the validation layer loaded and contains no validation errors for that process. The test uses reversible `wm size` changes because Thor ignored shell orientation requests; it reproduces the same original crash but is not a physical sensor-rotation test.

Regression coverage includes repeated real-window landscape/portrait resizes, a real Vulkan swap chain whose requested image count is deliberately increased within surface limits, and subsequent shrinking. The image-count allocation and feedback tests both failed before their corresponding fixes and passed afterward. Vulkan and D3D12 GPU tests pass. The complete Plume patch chain was applied to a clean temporary Git index and matched the checked-out source; the all-dependency bootstrap verifier cannot operate on this worktree's pre-existing shared tool directories.

This fixes the observed null semaphore and unbounded image-count feedback. It does not claim to redesign every presentation-lifetime path: Plume still lacks maintenance1 presentation fences, and queue idle alone is not a formal presentation-engine completion guarantee. See the [Khronos semaphore guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) and [swap-chain recreation sample](https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html).

### Steering range versus response time

Controller input previously mapped full stick to lean +/-1, while the original consumer at `822C6200` accepts +/-3.5. The default `SFR_RACE_LEAN_SCALE` is now **3.5**; explicitly setting 1 restores the prior range. Other axes remain normalized for tricks and gesture detectors. Six CPU tests cover full and partial steering, neutral reset, 20/30/60 Hz step durations, independent 1P/2P live readers, and the existing scale override/clamp.

There is no separate slower polling timer in this controller path: the manager samples input every game update and SDL events are pumped every guest present, including skipped renders. However, the original `822C63E8` filter combines the current sample with nine previous samples. Low FPS can stretch its response time. That filter and the simulation clock remain unchanged; physical event-to-steering latency and the user's preferred steering feel still require a device play test.

Normal test APK: `out/thor-race-perf/thor-rotation-steering-test.apk`, version `0.4.5-perf-thor-rotation-steering-test`, Android 10+ ARM64. SHA-256: `94d1bb2580c1a20b58eb4d5dea5b258367136fcb538956eb6eb16db8c7dd5446`. Packaging validates the release certificate, shader pack, native library contents, archive integrity and 16 KiB ZIP alignment. The final normal APK also survived six commanded size changes (all recorded rebuilds kept four images). Temporary test saves are separate from user saves; the validation APK and GPU-layer settings were removed before delivery.

### Follow-up: controller latency and Issue #31

The range change improved maximum steering but the user still reported delayed response. Investigation found two uses of the ten-sample filter in the controller path: source+4 in `822C6200`, and detector+72 in `822C8958` before writing steering result bytes +76/+78. Controllers now use the current sample at both boundaries. Original filter history is still maintained, gear/stance conversion is unchanged, and Camera/Kinect, body-angle filtering and unrelated consumers retain their original behavior. No simulation-clock or presentation-rate change is involved. Opt-in `SFR_RACE_LEAN_TRACE=1` records up to 120 differing original/applied values per guest thread; scripted input verifies these boundaries, not physical input-to-display latency.

The new tests failed before the corresponding overrides, then passed. They cover reversal/release/partial input, both player routes, the downstream steering result bytes, Camera/controller coexistence and sensor fallback. Windows and Android builds pass; existing compiler warnings remain.

Issue #31 also reports Windows Frozen Forest and intermittent World Grand Prix crashes, bright corners and failed left-side ring reach. The reach path had a concrete asymmetric bug: `RaceInput::write` overwrites left wrist X (+96) and other left-arm coordinates, but the native CatchLR/Catch detectors still interpreted them as geometry. Controller-specific adapters now emit the original left/right flags from the right stick, preserving Camera/Kinect geometry detection. Regression tests cover both detectors, both players, neutral/deadzone/arms-up and preservation of unrelated result flags. A later normal-time Windows gameplay trace confirms left-side pickups, including an 18-Ring chain, and a directed-input run completes GP mission 2 with 110 Rings / B rank. See the current controller reach and GP completion evidence in `docs/issue-31-investigation.md`; other courses, stances and the reporter's crash remain separate coverage gaps.

The crash and brightness reports have no attached log or screenshot. The existing offline-presence fix was already included in v0.4.5. A possible repeated-course resource issue is the renderer's retained `resolved_targets` map (full-resolution textures per physical resolve address); this is a code lead, not an established cause of #31. Confirm with a failing `game.log`, settings, GPU/API and the exact course/mode/mission. On Windows the next launch replaces `game.log`, so preserve it immediately after the crash. The offscreen target aliasing and depth-resolve limitations likewise need a screenshot/API comparison before attributing the bright corners to a particular pass.

Thor verification (`out/thor-race-perf/input-response-check*`) completed 14,400 presents and stopped at its configured limit, with Dolphin Resort visible at race time 01:06.61. The real guest trace confirms both filter overrides: full-left input -3.5 originally smoothed to -1.225, and mapped steering -100 originally smoothed to -35, are applied immediately at their respective boundaries. On release the original result still contained -29 while the applied result was zero. This is not a physical controller latency measurement or a Frozen Forest crash reproduction. Six `nui_race*` tests pass and independent review found no remaining actionable issues in the input changes.

Installed APK: `out/thor-race-perf/thor-input-response-test.apk`, version `0.4.5-perf-thor-input-response-test`; SHA-256 `7a23f6148a706bfbc05a1e9c556089701284c6aa444ffaaa1bea575c84d748f9`. Signature, native payload, shader pack and ZIP alignment checks passed. After the run, `debug.env` was absent, the game process had exited, and the device was returned to its normal launcher. The run used the separate benchmark save directory.

### Issue #31: Frozen Forest reproduction on the desktop

Tested the published **v0.4.5 Windows archive**, not the modified input build, on NVIDIA GeForce RTX 4090 at 1280x720 internal/output resolution. Executable SHA-256: `271bc31fc7ad2bfdc035ba5a3c186f025a21c9b82a30e91bc1ba496c9186a896`. Each backend started a fresh process with its own copy of the same save and its own native pipeline cache. The menu screenshots confirm **Free Race / Frozen Forest Standard**, using Sonic and the default gear. User saves were not modified.

- Vulkan: 18,000 presents, HUD **01:36.61**, **lap 2/3**; completed the first lap without crashing.
- D3D12: 18,000 presents, HUD **01:36.68**, **lap 2/3**; completed the first lap without crashing.
- Both processes ended with the expected diagnostic exit code 3 and `STOP present-limit`, not a crash. No `untranslatable=1` or searched device-loss/removal error was reported.

The reported Frozen Forest crash was **not reproduced in these two runs**. This does not cover World Grand Prix missions, other course variants, other GPUs/drivers or repeated races in one process. Reporter logs and exact settings are still needed. Evidence and repeatable script: `out/issue-31/run.py`, `frozen-vulkan/` and `frozen-d3d12/` (environment, full game log, process result, course-selection screenshot at frame 8400 and final race screenshot at frame 18000).

### Frozen Forest: Android ice-channel and audio investigation

The same course on Thor with `thor-input-response-test.apk` reproduces a
different failure from the reported Windows crash. The scripted neutral-input
run `out/thor-race-perf/frozen-water-check*` reaches the ice channel normally
(frame 13800, HUD 01:00.43), then places the camera inside the ice wall by
frame 14100 (01:12.16). At frame 16800 (02:46.39) it is still in that area on
lap 1. Rendering and the race clock continue. The run reaches its intentional
18,000-present limit, so this is not a whole-process freeze. The desktop
reference had reached lap 2 at 01:36.61 without player input.

The user also reports the music becoming noise in this area. Submitted/requested
audio-frame counts continue advancing, but those counters do not establish
valid sample data or correct device playback. The cause of the noise and its
relationship to the stuck rider are not yet established.

The first comparison disabling only `SFR_REALTIME_UI` ended during race loading
with `worker-memory-access`: guest 36, recorded function `82918418`, LR
`822A4C38`, r3 `0x71730000`, and reported fault address zero. With detailed entry
observation disabled, the recorded function is not sufficient to identify the
faulting instruction. This incomplete run is not evidence for or against the UI-timeline
hypothesis. Its log and system log are retained as `frozen-water-ui-off.log`
and `out/issue-31/ui-off-logcat.txt`; a fresh-process retry is required.

The retry `frozen-water-ui-off-retry*` completed the configured 18,000 presents.
It still remained in the ice channel at 02:23.39 on lap 1 (frame 16200), and
the user again reported broken music during that run. Disabling UI catch-up
alone therefore did not resolve either symptom.

The next control, `frozen-water-clock-off*`, disables `SFR_REALTIME_RACE` on
the same APK and records raw guest audio using `SFR_AUDIO_DUMP`. It reaches
18,000 presents normally: HUD 01:23.78/frame 17400 is moving through the
channel at speed 360; HUD 01:33.78/frame 18000 is visibly outside the channel.
Its 440.704 seconds of complete captured audio frames contain no nonfinite
samples. This control is slower in game time; it is diagnostic evidence, not
a shipped performance fix. The additional audio recording means frame-time
differences must not be treated as a clean performance benchmark.

Restoring the default realtime clock while keeping audio capture enabled
(`frozen-water-clock-on-audio*`) reproduces the stuck rider by frame 14400,
HUD 01:21.76, speed 74. The raw six-channel audio first contains a negative
quiet NaN (`0xffc00000`) at audio frame 63,446, sample 0, **338.378667 seconds**
into the dump. Each of the six channels contains 1,182,336 nonfinite samples
over the 365.306667 seconds of complete captured frames. This establishes
that invalid audio data exists **before SDL/device playback**; continuing
submission counters alone had hidden the corruption. The specific upstream
calculation that first produces NaN is still unknown. A shared origin with
the movement failure is supported by the clock comparison but not yet proven.

A Windows Vulkan v0.4.5 reference (`out/issue-31/frozen-desktop-audio/`) also
completed 18,000 presents with expected exit code 3. Its 300.53-second raw
audio capture has no nonfinite samples. Host speaker output was disabled
for that capture; the guest mixer and dump remained active. Normal finite
samples do not by themselves establish subjective audio quality.

Raw captures, 10-second statistics, finite PCM excerpts, the exact first bad
sample and the layout-checked analysis utility are under `out/issue-31/`.
PCM excerpts replace invalid samples with silence and therefore must not be
used as evidence of what the original NaNs sounded like. Partial trailing
audio frames on process exit are excluded from statistics. All test processes
have exited, temporary `debug.env` was removed, and Thor is back at its normal
launcher. No production clock or audio behavior was changed for this investigation.


Follow-up read-only actor probes (`frozen-rider-probe`, `frozen-math-probe`,
`frozen-stage-all-probe`) also reproduce nonfinite actor data. In the first
capture, slot 10's velocity-like vector at rider +240/+244/+248 becomes NaN
between presents 15180 and 15190. Other riders subsequently develop invalid
vectors. The sample blocks at rider +192 were initially treated as potential
pointers; this field is actually part of a transform and those incidental
blocks must not be interpreted as referenced objects. The useful records are
the rider itself and verified references such as +3208/+3220.

The guest double-pow probe (`82A55390`) records no nonfinite returns during a
complete 16500-present reproduction. A negative-base fractional-power theory
is therefore unsupported by this run. Actor corruption is not Android-only:
a private Windows diagnostic forces two 60-Hz frames per active race update,
with rendering unpaced, and reproduces the ice-channel failure and NaNs.
`frozen-desktop-step2` completes 18000 presents in 87.656 s, and the refined
`frozen-desktop-stage2` in 92.547 s; these are deliberately accelerated
reproductions, not realtime performance measurements. Their first detected
render-transform corruption is inside `822AAD60 -> 8228E378` on guest 7,
where gear rendering consumes the actor's already-invalid simulation data.
This is a propagation boundary, not yet the first invalid calculation.

A main-thread-only stage probe was deliberately stopped in menus and replaced
with one covering all workers. The full Android stage probe observes the same
boundary on guests 8/9. All probes live under ignored `out/issue-31`; none is
part of a production clock/audio behavior change. The next diagnostic tracks
entry history around the first invalid simulation-vector write on Windows.


## October 2 three-device stability follow-up

The earlier ice-channel and audio investigation now has a confirmed source:
boarding pursuit moved once per update while its target used elapsed time.
At low FPS, the rider could not catch the target, traversed the path in the
wrong state, and eventually used a zero heading. Targeted boarding scaling
and a zero-direction orientation guard address those two failures. A separate
Loading crash was traced to actor deletion racing a late worker batch.

See [Issue 31 investigation](issue-31-investigation.md) for the exact producer
chain, regression tests and per-device coverage. The corrected forced-step
desktop run finished Frozen Forest and captured 206.624 seconds of finite raw
audio. This is a stability result, not an FPS comparison. Remaining device,
locked-layout and Grand Prix coverage is recorded explicitly in that document.

### Android local function binding trial (not adopted)

A link-only Android experiment added `-Wl,-Bsymbolic-functions` to `libmain.so`,
using the same current source objects, shader pack, settings and independent
save fixture as the baseline. The candidate retains exported `SDL_main` and
the sampled strong guest hooks in both direct calls and `PPCFuncMappings`.
All `JUMP_SLOT` relocations fall from 19,811 to 316; native TLS descriptors
remain unchanged. This demonstrates less dynamic call indirection, not a
measured gameplay speedup.

Three unprofiled Thor runs used Dolphin Resort Standard, 720p, normal elapsed
race time and audio enabled. Whole 120-present summaries were selected within
HUD seconds 10–45 using visually read screenshot anchors, rather than comparing
identical present numbers after variable Loading durations:

| Run | Average FPS | Average frame | Maximum frame | Draws/frame |
|---|---:|---:|---:|---:|
| Baseline A | 21.95 | 45.57 ms | 83.99 ms | 779.8 |
| Local function binding B | 21.91 | 45.65 ms | 94.86 ms | 850.7 |
| Baseline A repeat | 21.36 | 46.82 ms | 82.81 ms | 798.1 |

The candidate lies inside baseline variation. Character encounters and course
position differ, and the selected complete intervals cover slightly different
parts of that window; no percentage improvement is supported. Pipeline compile
time is zero in these samples. GPU waits average 0.31–0.50 ms, while CPU work and
queueing remain substantial. Battery readings were 33–34 C, but the thermal HAL
reports unavailable, so these are not measurements of SoC temperature.

The flag is **not added to production CMake**. It also changes external function
interposition, a tradeoff that is not justified by the current runtime result.
The ignored `out/thor-race-perf/render-binding-comparison.json` records exact APK
hashes, anchors and intervals; `compare-render-binding.py` reproduces the table.

### Current renderer/stability CPU profile

`render-stability-profile` samples the current normal source objects using a
profileable manifest (APK SHA-256
`4cfd8700a8db83a0e227a24baa71391a139e8b3d8949b0317ad46378b590e17c`).
The 30-second `cpu-clock:u` recording contains 20,052 samples with zero loss;
symbols match native build ID `0efd8be56c7e1d2b949012ef7ee5dba0e7908dd2`.
The screenshot at present 13200 confirms Dolphin Resort Standard, lap 1,
HUD 00:40.93. Timing is elapsed-time, 720p, audio enabled. The bounded runner
reached its 16200-present limit normally. Sampling overhead means this is
attribution evidence, not a benchmark against the unprofiled A/B/A runs.

The main SDL thread accounts for 19.328 of 50.130 sampled CPU seconds across
all threads (38.55%). Its inclusive `native_draw` share is 24.63%, matrix
constant marshaling `82534038` is 5.08%, and leaf dynamic TLS resolution is
4.29%. Another guest worker accounts for 10.078 CPU seconds (20.10%), largely
inside the `827C9508 -> 827D7250` subsystem. Inclusive percentages overlap;
they must not be added or treated as a predicted frame-rate improvement.

Source inspection identifies `82534038` and `82533B90` as matrix/vector shader
constant packing, conversion and upload-callback dispatch. A private read-only
shape histogram is being used to establish the common descriptor and aliasing
cases before considering a targeted native replacement. No such replacement
or new TLS policy follows from this profile alone. Matching data, symbols,
thread attribution and caller summaries are retained under ignored
`out/thor-race-perf/render-stability-profile*`.

### Constant layout experiment (no production replacement)

The normal-clock Android shape probe recorded 9,885,809 calls over five
intervals without unreadable metadata, overflow, or probe errors: 4,289,977
matrix calls and 5,595,832 vector calls. Matrix calls were float column-major
4x4 (3,406,686) or 3x3 (883,291), with source and scratch pointers unequal.
This does not prove absence of partial overlap. The vector path includes
267,088 structure-forwarding calls, which cannot be treated as plain vectors.
Raw and aggregate evidence is in `out/thor-race-perf/constant-shape*`.

An isolated native matrix-packing helper passes bounds, overlap, padding,
signed-zero, infinity and NaN tests. A Windows read-only oracle then compared
its predicted bytes against the original function's scratch result. By present
15000 it had checked 3,855,430 calls containing 6,729,304 matrices; the run
continued through the 15500 observation cutoff and stopped at 16200 without
mismatch. Unsupported/overlapping/cross-page cases use the original path.
The executable SHA-256 is
`4bbd3f8d1f07255758f3aface0c9a65eb032811ce0ca4947bcba4b65a746fc6b`.
The original remained the only guest-memory writer and upload-callback caller.
This validates candidate packing for observed inputs, not a replacement's
callback ABI, all floating-point side effects, or performance. No fast path
has been adopted.


### Android prime-core placement trial (not applied)

A private same-APK control requested CPU 7 for the main race thread after
present 13000, only if that CPU was already in its allowed affinity mask.
Thor exposes CPU 7 as its highest-capacity core, but the actual game thread's
query succeeded with CPU 7 absent at the request point. The candidate therefore
made no affinity change. The first baseline and candidate both reached their
16200-present limits; the planned third run was omitted because there was no
treatment to compare. The reported errno after the successful query is stale,
not evidence of a failed system call or a permission denial.

No affinity policy or performance gain is adopted. APK SHA-256:
`5d8ab28757762fa99ec414556a4e0af0f7cfe9dcd51100aedfd50938bc3d4558`.
Logs and the explicitly inconclusive comparison are under ignored
`out/thor-race-perf/core-*`. This experiment preserves the device's existing
allowed CPU range and does not override its power or scheduling policy.

A subsequent read-only `/proc/<game-pid>/task/*/status` check during a different
Expert run reports 0–7 for all threads, both in menus and in the race. Thus the
single skipped request does not establish a permanent device restriction or
its cause. The probe did not log the denied mask or TID; a repeat should include
those fields before drawing a scheduling conclusion.

### Retiring completed guest thread storage

A 128-worker regression exposed the fixed 96-slot session quota: closed, completed
threads never released their guest-address slots. Equal-size storage can now be
reused after completion/join with no open guest handle or explicit reference.
TLS address registration is retained while its contents are reset. Old native
exit handles stay valid for existing waiters. Windows and Android regressions
pass; this is a long-session stability fix, not a measured frame-rate gain.
The GP mission-2 retry stress stayed below the old quota (highest guest ID 41),
so it does not attribute the reporter's GP crash to this defect. See
[the investigation](issue-31-investigation.md) for reproduction and limits.

### Guarded matrix reads: no demonstrated race improvement

A private replacement of `82534038` preserved its scalar load/store order,
floating-point operations, checkpoints and callback. Only aligned 4x4 source
reads on a complete ordinary page, under non-detached guest execution, used
a shared range check. All other accesses retained their original checks.
The experiment passes the 140 independent contract cases plus 768 boundary,
448 special-memory and eight real slow-save prologue-fault comparisons on
Windows and Thor. These fixtures do not replace testing the actual game.

The strict guard reduced standalone ARM64 call time by 4.81%, 11.90% and
23.70% for one, four and 64 matrices. The unchanged 3x3 control varied by
under 0.70%. This is not a frame-rate result: the original routine accounted
for only 5.08% inclusive main-thread CPU in the earlier game profile.

An original/candidate/original comparison then used the same private APK,
Dolphin Resort Standard, 720p, normal elapsed race time, audio enabled and
independent copies of the same save fixture. Screenshots aligned each sample
to HUD seconds 20–80; measured HUD/wall-time anchor offsets differed by less
than 0.08 seconds within each run. No drawing was skipped.

| Run | Average FPS | Average frame | p95 frame | Draws/frame |
|---|---:|---:|---:|---:|
| Original A | 21.41 | 46.71 ms | 63.70 ms | 901.6 |
| Guarded reads B | 21.81 | 45.85 ms | 61.10 ms | 869.6 |
| Original A repeat | 22.02 | 45.41 ms | 61.19 ms | 863.9 |

The candidate is within baseline variation. Opponent, item and course-position
differences also change the draw workload. No whole-game speedup or stall
reduction is established, so this replacement is **not adopted**. All three
runs reached the 16200-present bound normally; these are sampled races, not
three verified race completions. Each source save was unchanged and the exact
ordinary v0.4.6 APK and normal launch settings were restored after each run.

Private APK SHA-256:
`8bc47978cab7668ab139705e803ed97959939d455e897b582717ea4e23d4b09f`.
Reproducible comparison, screenshot anchors, library/source hashes and raw
logs are retained in ignored `out/continuation-20261002/matrix-game` and the
three `thor-matrix-*` directories. The separate renderer lifetime correction
is present in both A and B, so it is not the variable tested here.

### Android performance-hint experiment (not adopted)

A private Thor / API 33 platform probe could create a Performance Hint session
with a preferred update period of 16,666,666 ns. GameManager returned UNSUPPORTED
at probe startup both with and without the manifest game category. Neither
observation establishes a performance benefit or permanent device capability.

A separate same-APK off/on/off comparison reported measured main-thread work
to Android, using a 16.67 ms target. The work interval excluded GPU presentation,
frame pacing and screenshots. Only API 33 functions resolved dynamically were
used; the API 29 library acquired no hard Performance Hint imports. The normal
clock, audio, rendering quality, affinity and power-mode settings were retained.

| Main-thread hints | FPS | Mean frame | p95 frame | Draws/frame |
| --- | ---: | ---: | ---: | ---: |
| Off A1 | 21.88 | 45.71 ms | 62.73 ms | 892.1 |
| On B | 21.64 | 46.21 ms | 61.95 ms | 912.6 |
| Off A2 | 21.44 | 46.65 ms | 61.42 ms | 922.7 |

Screenshots align Dolphin Resort Standard at HUD 20–80 seconds, with clock
anchor spread below 0.07 s in each run. Draw workloads differ, and the candidate
falls inside baseline variation: no FPS improvement is established. All 14,999
hint reports succeeded, the session closed, and frame-boundary CPU samples
still rarely landed on CPU 7. API success alone is insufficient reason to
adopt the integration.
Battery temperatures were 34–35 C; no usable thermal-HAL readings were available.

All three runs reached the 15000-present limit with independent saves unchanged;
lap progression is verified, race completion is not. The exact normal v0.4.6 APK
was restored and verified after each run, with no debug.env or game process left.
No production hint or manifest change was adopted. Private provenance, logs,
HUD anchors and comparison are under `out/continuation-20261002/hint-game` and
`thor-hint-*`. The APK SHA256 is
`3b205f498b90fd9c0cd0bbcc58d5b034c18ff33a42973a23e6c02f6be5486516`.

### Repeated raster state: frequent, but a small measured cost

A private observer retained every viewport/scissor submission and sampled every
64th `apply_raster` call on Thor. In Dolphin Resort Standard, presents 13200–14400
(HUD 00:41.39–01:33.84), it observed 1,034,692 calls; 99.65% matched the preceding
state and command-list generation. The 16,167 sampled calls averaged 174 ns,
estimating 0.150 ms per frame, with estimates of 0.116–0.211 ms across 120-frame
intervals. The instrumented frame mean was 43.715 ms.

Repeated state is therefore not evidence of a large performance opportunity.
These wall-clock samples include timer overhead and OS scheduling, and periodic
sampling can be biased. Equality also does not establish safe reuse around all
external custom/blit operations. No raster cache was implemented, and no FPS
improvement is claimed. A future cache would need restoration regressions for
new lists, clears, resolution scaling, avatars and presentation.

The normal-time, 720p, audio-enabled run reached its 15000-present limit after
355.922 seconds, with lap 2 visible; this was not a complete-race test. The source
save was unchanged, and the exact normal APK and launch settings were restored.
Private observer source/diff, hashes, screenshots and reproducible summary are
under `out/continuation-20261002/raster-probe` and `thor-raster-observed`.
APK SHA-256:
`1286f9b4e3be703920f8fe8e99529dec30162b1c9a5f629567ee72beecdb18dc`.


### D3D12 distinct-course regression at normal elapsed time

The renderer allocation-lifetime correction was exercised on RTX 4090 / D3D12
at a 60 Hz cap, 720p and normal elapsed race time with audio enabled. One process
completed Frozen Forest Standard (Replay at present 34800), returned to course
selection, selected Dolphin Resort Standard (40800), and reached its Replay
screen (67800). The 80000-present test ended through the expected present-limit
stop after 1335.422 seconds, without its host timeout. The independent source
save was unchanged. Executable SHA-256:
`c116aa9662e44e8d802420535c2f1a2d0bbd997f4378dd5cb2b05c0b25092844`.

This adds distinct-course D3D12 coverage to the earlier Vulkan transition and
Thor course completions. The previous D3D12 run had selected Frozen twice;
that result remains a same-course reload test. This is functional coverage,
not an FPS benchmark, exhaustive image/audio validation, or reproduction of
the reporter's Issue #31 crash. Provenance, raw log, visual evidence and source
hash checks are in ignored `out/continuation-20261002/resolve-two-courses-d3d12-route2`.


### GPU timeline spans: low CPU fence waits do not rule out GPU cost

A private Thor observer adds two Vulkan timestamps around each existing
presentation command list. Its two query pools follow the existing list/fence
slots. Query reset happens before a render pass, results are read without WAIT
only after the corresponding existing fence wait, and no draw or synchronization
wait is removed. Queue timestamps report 48 valid bits and a 52.0833 ns period;
there were zero query errors in either run.

Two runs used the same observer APK, normal elapsed time, audio and independent
Dolphin Resort Standard save copies. Physical internal resolutions were verified
in the log. Complete 120-present intervals were selected inside HUD seconds
45–80 using screenshot anchors; anchor offset spreads were 0.056 s and 0.004 s.

| Internal resolution | GPU list span/present | Frame time | CPU fence wait | Draws/frame |
| --- | ---: | ---: | ---: | ---: |
| 1280x720 | 39.63 ms | 48.45 ms | 0.309 ms | 1119.5 |
| 640x360 | 26.19 ms | 42.62 ms | 0.037 ms | 838.2 |

Substantial GPU timeline spans coexist with small CPU waits because work can
execute while the CPU records the next frame. The earlier low `gpu_wait_ms`
observations therefore do not establish a CPU-only bottleneck. The lower
resolution sample has shorter GPU spans, but also a different position/opponent
and draw workload. These two observations do not establish a reproducible FPS
improvement or quantify the benefit of lowering resolution alone.

Bottom-of-pipe timestamp spans can include stalls and instrumentation effects;
they exclude independent upload/other queue lists and are not exact GPU
utilization. CPU stage timers overlap these spans and must not be added. Further
work should attribute GPU passes/shaders and compare matched workloads before
adopting an optimization. No runtime optimization, timestamp observer, or default
resolution change was added to production.

Both runs reached their 15000-present bound and showed lap progression, not race
completion. The exact ordinary APK was restored and hash-verified after each,
with original fixture saves unchanged and no debug override left. Private source,
diff, build/APK validation, raw logs, screenshots, and reproducible summaries are
under ignored `out/continuation-20261002/gpu-time-probe`, `thor-gpu-time-observed`
and `thor-gpu-time-half`; `compare-gpu-time-resolution.py` reproduces the table.
Observer APK SHA-256:
`96e9b2480951aa3cdad888ef6e98673fcf846f14945f1630eb1c70eaad4b3663`.


### GPU pass attribution and oversized offscreen compatibility path

Two private observers timed only existing Vulkan render-pass boundaries and
retained all original drawing. Pass timestamps were sampled every120 presents
from12000 onward, using the existing list/fence retirement; results introduced
no extra host wait. The coarse run completed15000 presents in350.938 seconds.
In11 sampled race frames it measured36.94ms/list,35.91ms inside passes and1.03ms
outside. The initial scene pass averaged8.38ms; repeated one-quad stages dominated
the remainder. This directed the next observation toward postprocessing rather
than a blanket shader-constant rewrite.

The detailed run added first-draw viewport/scissor state and guest resolve
sizes, then completed15000 presents in355.407 seconds. Presents13200–14400
showed Dolphin Resort progression from HUD00:40.77/lap1 to01:31.90/lap2.
Across11 sampled frames and267 passes:

| Observed work | GPU timestamp span per sampled frame |
| --- | ---: |
| All presentation lists | 37.32 ms |
| Inside existing render passes | 36.32 ms |
| Outside render passes | 1.00 ms |
| Initial scene pass | 9.16 ms |
| Single-draw, four-vertex, non-indexed passes combined | 25.18 ms (67.47% of list span) |

All223 single-quad passes used a1280x720 viewport and scissor, with no changes
within those passes. The same frames resolved231 small guest targets sized
55x45,110x90 or112x92. Resolve order and pass order matched after excluding the
final host blit in every sampled frame; individual target/pass associations
remain sequential inference because GPU rows do not carry unique target tags.
A full viewport does not by itself prove every pixel was shaded: quad positions
were not captured. It does establish that the host viewport was not reduced to
those guest target dimensions.

The source explains this compatibility path: `GuestGraphics::set_viewport`
clamps origin-zero offscreen passes to the native framebuffer, while
`NativeRenderer::adopt_resolved_target` allocates/copies the full presentation
size. Merely clamping viewports to guest target sizes can break saved viewport
state, screen-space coordinates, resolved sampling, exposure and split views.
A correct smaller-target implementation would need these contracts together.

A narrower candidate is avoiding depth/stencil attachment traffic when neither
is used by a draw. Current native framebuffers attach D32_FLOAT_S8_UINT with
LOAD/STORE operations even through postprocessing. Eligibility is now measured;
savings are not. Do not treat this as an implemented fix. Preservation
oracles, compatible pipelines, clear/custom-draw transitions and A/B/A are laid
out in `docs/superpowers/plans/2026-10-02-gpu-postprocess-cost.md`.

Both observer runs had zero query errors/overflow and reached their normal
present bound. Independent source saves remained unchanged; the exact ordinary
APK and launch settings were restored and subsequently checked again. Timestamp
and callback overhead, GPU stalls and periodic sample bias remain limitations.
No observer or performance candidate was added to production. These were partial
races, not two additional verified finishes. Results and reproducible scripts
are under ignored `out/continuation-20261002/gpu-pass-probe`, `gpu-pass-detail`
and their matching `thor-*` cases.
Detailed observer APK SHA-256:
`efcbae9429fbd91d90c7ce85d15b536ba82a4080ed8510eb8d78c963d0362037`.

A subsequent private observer also captured each guest draw's actual depth and
stencil flags. In 11 sampled race frames, all227 single-quad passes disabled
depth testing, stencil testing and depth writes. These stages spanned25.32ms
per sampled frame,69.94% of the36.20ms overall GPU list spans. They qualify for
an unused-depth experiment; this does not mean depth traffic accounts for that
entire cost. The11 final host blits were the only draws without guest state.
No query errors or overflow occurred. The15000-present run ended in350.563s,
with source save and ordinary APK/settings restoration verified. Frame14400
shows lap2 at89.05s; this partial race is not additional finish coverage.

The native resolution test now includes a depth/stencil preservation oracle:
depth-tested geometry, blended color-only work, stencil-only masks, selective
depth clears and repeated transitions at50/100/200% render scale. Vulkan and
D3D12 pass. Temporary test-only depth loss and stencil loss each fail the
expected pixel assertion on both backends; restoring the test source restores
both passing results. This verifies the oracle's sensitivity, not an optimized
attachment path. Custom/VRM and an explicit assertion that such a path is used
remain prerequisites for a future candidate. Evidence is in
`out/continuation-20261002/gpu-pass-depth/eligibility.json` and
`depth-oracle-validation.json`. Observer APK SHA-256:
`5c8c00efd64fc21c1ed4c9cb8c0e7abcd656576831539e18e6bfa4fe13c2eda4`.

### Unused-depth candidate: measured and not adopted

The subsequent private Vulkan candidate omitted the depth attachment only when
both depth and stencil testing were disabled, using compatible pipeline formats
and explicit dependencies when switching framebuffers. The preservation oracle
was extended to assert the actual attachment, exercise a synthetic model draw,
resolve/default-record paths and the following frame. Both Vulkan and D3D12
passed at 50/100/200% scale. The Khronos validation layer was unavailable on the
test host; these are pixel/state checks, not a validation-layer clean bill.

One APK was tested off/on/off on Thor at 720p, with audio and normal elapsed
game time. Matching complete measurement intervals inside HUD seconds 45–80:

| Run | Observed FPS | GPU list span/frame | Draws/frame |
| --- | ---: | ---: | ---: |
| Baseline before | 23.306 | 37.267 ms | 867.0 |
| Candidate | 23.257 | 37.362 ms | 831.7 |
| Baseline after | 21.994 | 38.895 ms | 952.6 |

The candidate fell inside baseline variation and did not reduce GPU time versus
the first baseline. Workloads differ despite matching HUD time, so these runs
do not isolate every source of variation. They provide no reason to adopt this
change. All candidate production changes and candidate-only tests were removed;
the earlier baseline preservation oracle remains. The ordinary APK/settings and
source fixture were verified after every run. Private source snapshots, logs,
screenshots and reproducible analysis remain in
`out/continuation-20261002/color-only-probe`, including `comparison.json` and
`retired-candidate.json`. These runs cover partial races, not additional finishes.

### Issue #33: checkpoint interval investigation

The measurements below record the investigation **before PRs #35 and #36**.
Their references to a default of 32 or a pending render-thread decision describe
that historical test build. For v0.4.7, Windows adopts checkpoint interval 256;
other platforms retain 32. D3D12 enables the render worker by default, while
Vulkan retains synchronous recording. See [the adopted checkpoint policy](checkpoint-interval.md)
and [render-thread behavior and limitations](render-thread.md). The private
experiments below are evidence, not additional enabled release features.

[knuckleslee's paired report](https://github.com/YuutaTsubasa/Free-Riders-Recompiled/issues/33)
compares six interleaved rounds on three Windows machines using a modified
v0.4.5 fork. Recomputing the supplied rounded values gives median checkpoint
256/32 throughput ratios of 1.0602 (i5-3470), 1.0701 (i7-6850K) and 1.0782
(Ryzen AI Max+ 395), with all six pairs positive on each machine. This supports
testing a longer checkpoint interval. It does not establish the same gain on
the current version or on Android: the report used D3D12, disabled audio and
combined several other fork changes. Its elapsed-game-time setting is unknown.

`SFR_CHECKPOINT_INTERVAL` now permits a controlled comparison from 1 through
4096. The default remains 32; missing, malformed and out-of-range values use
that baseline. The effective interval is logged once. Configuration is read in
the existing slow permit path, leaving generated function-entry/loop code
unchanged. Cancellation and urgent handoff are checked on each permit call;
the ordinary quantum clock is sampled every 64 calls. An interval counts guest
entries/checkpoints, not a guaranteed number of milliseconds. A larger value
must therefore be evaluated for responsiveness as well as throughput.

Tests cover numeric parsing, actual mixed entry/loop cadence and cancellation
through a real execution lease, alongside the existing scheduler tests. Both
test targets pass. A bounded Windows D3D12 Frozen Forest run at 256, with audio
and normal elapsed time, reached the Replay screen and stopped at 36000 presents
in 602.578 seconds. Its 60 FPS cap makes this functional evidence only. Audio
was enabled, but noise quality was not independently listened to. A clean
Windows executable was rebuilt after the rejected GPU experiment was removed.

The larger render-thread proposal remains separate: the report shows its main
gain on the i5, and importing it needs current-version resource-lifetime and
device validation. The already-upstream targeted suspend notifications receive
additional supporting evidence from the report. No unchecked memory access,
pipeline reuse or register-localization changes were imported. Exact issue and
branch snapshots, source hashes and test logs are retained under ignored
`out/continuation-20261002/issue33`.

The clean same-APK Thor comparison completed in order 32/256/32, each at its
15000-present bound. In matched HUD seconds 45–80, complete measurement windows
gave 20.249/23.681/23.345 FPS, with 1517/950/866 draws per frame respectively.
The candidate is 1.44% faster than the later baseline, while the two baselines
differ by 15.3%. The first run's much larger draw workload prevents attributing
its difference to the checkpoint interval. GPU spans were 38.744/38.313/36.980ms;
P95 frame times were 69.787/54.715/56.351ms. This one sequence does not establish
a repeatable Android benefit or justify a default change. All three runs passed
restoration checks; screenshot HUD progress agrees with elapsed host time within
0.033s between anchors. These are partial races. The thermal HAL was unavailable;
battery temperature was 35C, which is not a substitute for SoC temperature.
`issue33/compare-checkpoint.py` reproduces the measurements and records log/image
hashes in `checkpoint-comparison.json`. Default 32 remains on all platforms
pending a current-version Windows throughput comparison and further device evidence.

The current-version Windows comparison is now available on an i9-14900KF /
RTX4090 (D3D12,720p,balanced power plan,driver596.49). One private executable
preserved the known 60-FPS menu script through present13200, then removed the
host cap for exactly120 wall seconds. Normal elapsed race/UI time and audio
remained enabled; every required draw was retained. Source, ordinary object and
executable hashes were unchanged by the private build. All three independent
save/cache runs stopped at their expected bound and preserved the source fixture.

Matched HUD seconds45–80, with frame instrumentation enabled throughout:

| Checkpoint interval | FPS | P95 frame time | Draws/frame |
| --- | ---: | ---: | ---: |
| 32 before | 80.931 | 17.188 ms | 840.6 |
| 256 | 81.305 | 16.215 ms | 902.4 |
| 32 after | 75.165 | 17.559 ms | 917.1 |

The candidate is only0.46% faster than the first baseline and8.17% faster than
the last, while the baselines differ by7.67%. Different autonomous trajectories
and draw workloads remain confounders. One sequence does not establish the
report's6–8% benefit on this current build/host or justify changing the default.
It does establish a usable uncapped measurement path without speeding up the
game: HUD anchor offsets agree within0.025s, selected frames have zero pacing
wait, no clock clamps and no frames over50ms. XAudio2 submission cadence stays
187.46–187.52 blocks/s, consistent with256 samples at48kHz; this does not verify
audible quality or playback underruns. These are partial races, not finishes.

The private methodology, source diff, source/executable hashes, logs and six
inspected screenshot anchors are under ignored
`out/continuation-20261002/issue33/windows-throughput-*`.
`compare-windows-throughput.py` reproduces `windows-throughput-comparison.json`.
Further work needs more comparable scene workloads and additional paired runs
or an available slower Windows machine; an affinity-limited host could be a
stress comparison, but must not be represented as an actual i5/Ally result.

A follow-up held the ordinary brake input from present 12000, keeping the rider
at the Dolphin Resort starting line. Nine inspected screenshots across the same
32/256/32 executable sequence show speed 000 and a stable camera while normal
HUD time advances. AI racers continue moving; this reduces route variation but
does not make the whole workload deterministic. No game-state or speed patch
was used. The same 120-second uncapped bound, audio, all draws and independent
save/cache setup remained in place.

| Checkpoint interval | Stationary FPS, HUD 45–80s | P95 frame time | Draws/frame |
| --- | ---: | ---: | ---: |
| 32 before | 86.923 | 12.555 ms | 670.0 |
| 256 | 90.162 | 12.187 ms | 678.8 |
| 32 after | 87.380 | 12.499 ms | 669.7 |

The candidate is 3.73% and 3.18% faster than the two baselines, whose difference
is 0.53%. Two other preselected HUD windows (35–55s and 55–75s) also favor 256
over both baselines, by 2.80–4.24%. The candidate renders slightly more draws,
not fewer. This is positive evidence for this stationary desktop workload,
but one sequence is not a replicated active-race, low-end Windows or Android
result. Keep default 32 pending further paired runs and responsiveness coverage.

All runs stop at the expected bound in about 342 seconds and preserve the source
fixture. HUD/host anchor offset spread is at most 0.017s; selected frames have
zero pacing waits, no clock clamps and no frames over 50ms. Audio submission
cadence remains 187.48–187.52 blocks/s. Ordinary source/object/executable hashes
were freshly verified unchanged, and no game process remains. Reproduction
scripts, settings, image/log hashes and restoration checks are in ignored
`out/continuation-20261002/issue33`: `stationary-method.md`,
`run-windows-stationary.py`, `compare-stationary.py`,
`stationary-comparison.json` and `stationary-restoration.json`.

The reverse-order replication (256/32/256) used three new independent cases
with the same binary and settings. Fresh HUD anchors and screenshots confirm
the same stationary scene and correct elapsed time:

| Checkpoint interval | Stationary FPS, HUD 45–80s | P95 frame time | Draws/frame |
| --- | ---: | ---: | ---: |
| 256 before | 92.877 | 11.887 ms | 667.6 |
| 32 | 87.278 | 12.524 ms | 667.2 |
| 256 after | 91.517 | 11.955 ms | 670.0 |

Both candidate runs outperform the intervening baseline, by 6.42% and 4.86%.
The other two predefined windows are also positive (3.30–6.12%). Across both
sequences, the fixed-scene Windows D3D12 result is consistently positive;
variation in the size of the gain remains. Each reverse run ended normally at
about 342.141s. Selected windows contain no clock clamps or frames over 50ms,
and audio submission cadence stays 187.48–187.54 blocks/s. All six binaries,
normalized settings and source fixture hashes match. Normal outputs are unchanged.
Analysis is reproduced by `compare-stationary-reverse.py`, with cleanup and
cross-sequence checks in `stationary-sequences-verification.json`.

The entry-state regression now also exercises real urgent leases through mixed
inline function-entry/loop checkpoints at intervals 1, 32, 256 and 4096. Even
with a one-hour ordinary scheduling quantum, an urgent waiter must run at the
next permit boundary. A private mutation that delays that handoff fails the new
assertion; the unchanged production scheduler passes `guest_entry_state` and
`guest_execution` (2/2, 3.27s). No additional CI target is needed. This establishes
entry-count ordering, not a wall-clock input/audio latency bound, reservation
duration bound or audible playback quality.

Default 32 still applies. Vulkan and active input/audio follow-ups are documented
below; D3D12 stationary throughput alone does not establish that broader result.
These experiments do not change the
inconclusive Android evidence or resolve the reporter's separate Issue #31 crash.

The same stationary protocol was then run on Windows Vulkan (32/256/32), using
the same executable as the D3D12 experiments. HUD seconds 45–80 gave:

| Checkpoint interval | Vulkan FPS | P95 frame time | Draws/frame |
| --- | ---: | ---: | ---: |
| 32 before | 78.474 | 14.167 ms | 670.9 |
| 256 | 80.585 | 13.821 ms | 667.8 |
| 32 after | 77.257 | 14.268 ms | 670.8 |

The candidate is 2.69% and 4.31% faster than the baselines; the baselines differ
by 1.58%. Both other predefined windows are positive (2.04–4.38%). Candidate
draw count is about 0.46% lower, so this is still approximate workload matching.
All three bounded runs finish in 342.468/342.266/342.422 seconds. Nine screenshots
confirm the stationary scene and speed zero; HUD/host offsets agree within
0.017s. Selected frames have zero pacing waits, no clock clamps and no frames
over 50ms; audio submission cadence is 187.48–187.52 blocks/s. Logs, anchors and
reproducible analysis are under `issue33/windows-stationary-vulkan-*` and
`stationary-vulkan-comparison.json` in the continuation evidence directory.

This supports investigating a Windows checkpoint change on both backends, but
is one Vulkan sequence on this host. It does not establish lower-end Windows,
Android or physical controller latency. The default remains 32.

A separate private observer then compared intervals 32 and 256 on Vulkan with
normal time, audio and all draws. Ordinary scripted controls use wall-clock
timing: nine left/right/release transitions while braking at the start line,
then nine after releasing the brake. Both original lean consumers received all
18 transitions with the expected signs, scales and neutral releases. Each
transition reached each consumer within one present after the first logged poll.

| Interval | Stationary nominal-to-consumer median / maximum | Moving nominal-to-consumer median / maximum | First-poll-to-consumer maximum |
| --- | ---: | ---: | ---: |
| 32 | 7.057 / 15.039 ms | 12.063 / 15.393 ms | 13.889 ms |
| 256 | 7.628 / 14.003 ms | 10.705 / 13.329 ms | 7.145 ms |

Values use the actor consumer; the separate source consumer follows the same
transitions about 0.014–0.024 ms earlier. This small sample checks synthetic
script-to-consumer behavior, not physical controller or display latency, and
does not establish that the candidate improves responsiveness. Screenshots
confirm the stationary start and subsequent moving race. Instrumented runs
are not used for throughput comparisons.

Each case contains 121 audio-health reports across about 120.26 seconds. Both
have zero XAudio2 engine-glitch increments, empty-queue observations, full-queue
drops, failed submissions and nonfinite samples. Sampled queued buffers range
from 3–5 at interval 32 and 1–3 at 256; these observations do not prove a latency
improvement. Above-unity samples occur in both runs (498/610; peaks 1.577/1.582),
which alone does not establish corruption in floating-point mixed audio.
These counters do not establish audible correctness of the game's samples.

Both runs finish at the expected bound (342.329/342.141 seconds). Binary and
normalized settings match within the pair; source saves, ordinary sources,
objects and executable hashes are unchanged. No test game process remains.
Evidence and reproducible analysis are in `issue33/input-audio-vulkan-*`,
`analyze-input-audio.py` and `vulkan-observer-verification.json`. The analyzer
accepts scientific-notation timestamps, including the initial `1e-07` sample;
no raw samples were removed to pass its common-clock check.

The same private observer subsequently completed the D3D12 pair with the same
script, isolated saves and normal-time settings. Both consumers again receive
all 18 left/right/release edges:

| Interval | Stationary nominal-to-actor median / maximum | Moving nominal-to-actor median / maximum | First-poll-to-actor maximum |
| --- | ---: | ---: | ---: |
| 32 | 7.947 / 14.119 ms | 7.749 / 12.021 ms | 14.034 ms |
| 256 | 10.017 / 11.787 ms | 7.766 / 12.278 ms | 10.950 ms |

One baseline stationary edge crosses two presents (14.034 ms); the other
baseline edges and all candidate edges cross at most one. Present counts are
retained alongside wall-clock times, not treated as a backend-independent
latency bound. These small samples show no missed edges or obvious delay
regression; they do not prove improved responsiveness. The stationary median
is higher in the candidate, while the maxima and moving medians differ in
other directions. Source-consumer results are recorded separately.

Each D3D12 case has 121 audio reports across 120.126/120.077 seconds, with zero
engine-glitch increments, empty-queue observations, full-queue drops, failed
submissions and nonfinite samples. Both sample 1–3 queued buffers. Above-unity
samples total 562/1207, with peaks 1.642/1.559; different autonomous race paths
preclude treating these counts as an audio-quality comparison. Screenshots
confirm stationary speed zero at present 13200 and moving races at 18000.
Both runs exit at the intended bound (342.187/342.062 seconds).

`verify-vulkan-observer.py --include-d3d12` checks all seven new Vulkan/D3D12
cases: matching binaries and normalized settings within each group, log hashes,
actual backend selection, unchanged source fixtures and ordinary build hashes,
all observed input edges, and no remaining game process. Its report is
`all-backend-observer-verification.json`; all four observer analyses are kept
in `input-audio-comparison.json`. Ordinary launch settings remain untouched.

The pre-integration current-host checks covered both graphics backends. That
test build retained default 32 pending the later Windows decision; a slower Windows
host would strengthen coverage. Android evidence remains inconclusive;
Issue #31 and GP mission 3 remain separate work.

## Post-v0.4.7 CPU investigation

A 35-second Windows CPU sample, resolved against its exact executable map,
recorded 68,680 samples with no lost ETW events. Of the process samples, about
35% belonged to the guest main thread and 31% to the D3D12 recording worker.
Most worker stacks passed through its empty-queue `SwitchToThread` loop.
These are sampled attribution figures, including some kernel/interrupt
activity, not percentages of removable work.

An isolated candidate reduces only that worker's yield budget from 2,000 to
32 before using its existing condition-variable handshake. Producer waiting,
queue ordering, drawing and game time are unchanged. The candidate-linked
presentation oracle passes for D3D12 and Vulkan, including queue wrap, drain,
shutdown and failure cases. Independent scope and synchronization reviews
found no change-induced correctness defect.

The following sequential Ally X D3D12 tests use an independent save per run,
the same stationary Dolphin Resort scene, AC power and the Turbo power plan.
Menus are capped; the private harness then runs an uncapped, normal-time race
for 120 seconds. Frame metrics use the middle 100 seconds; process/thread CPU
deltas cover approximately 85 seconds inside that window. Diagnostic logging
is identical across these cases. CPU ms/frame is summed across process
threads; it is not elapsed frame time.

| Run / worker budget | FPS | Process CPU ms/frame | Main CPU ms/frame | P95 / P99 frame ms | Frames >50 / >100 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| A / 2000 | 70.87 | 29.72 | 9.70 | 16.09 / 18.91 | 3 / 0 |
| B / 32 | 73.73 | 24.26 | 9.72 | 15.11 / 16.61 | 5 / 1 |
| C / 2000 | 74.70 | 28.63 | 9.47 | 14.96 / 16.30 | 0 / 0 |
| D / 32 | 72.53 | 24.80 | 9.82 | 15.43 / 17.12 | 5 / 1 |
| E / 2000 | 74.51 | 28.44 | 9.44 | 15.09 / 16.62 | 1 / 0 |

The experiment remains **on hold**. Process CPU/frame falls about 13–18%,
but main-thread CPU does not improve and FPS gains are not established against
baseline drift. Recording/draw cost also increases slightly. Both candidate
windows include a frame above 100 ms, versus none in the baseline windows;
baseline A and E still have substantial shorter stalls. This does not prove
that the wait budget causes the long frames. Their extra wall time appears
mostly under main-thread execution ownership, which can include host
descheduling. Subsequent scheduler traces distinguish running, runnable and
blocked time but did not reproduce the large candidate stalls. Production
retains the original budget.

Screenshots in D show the race clock advancing about 66.08 seconds while
logged elapsed time advances 66.0768 seconds between presents 14400 and 19200.
They confirm the full stationary scene and normal race-clock progression;
they do not validate every animation or course. Neither reduced CPU cost nor
passing correctness tests establishes a handheld FPS or battery-life gain.
Default Vulkan and Android do not use this D3D12 worker.

Local evidence is under `out/cpu-profile-v047/`: exact source/object/executable
hashes, five completed case logs, frame and CPU summaries, screenshots, and
review records. The original `baseline-a` menu-only attempt is rejected;
table A is the corrected `baseline-profile-a`. `SFR_PROFILE=1` enables the
local player profile and is not a performance-logging switch. Raw system
traces remain private and are not release assets.

A separate four-line ordinary-page memory-preflight optimization has passed
1,208 baseline/candidate state comparisons and the existing guest-memory,
pending-write and vector-memory suites. It retains all slow paths, write
watches, reservations and pending-output semantics. The linked diagnostic
keeps the original worker budget so its effect can be measured independently.
An initial baseline/candidate/baseline sequence measured 9.463 / 9.144 / 9.454
main-thread CPU ms/frame. This roughly 3.3% reduction is promising, but is not
on its own sufficient for acceptance. The last baseline also had a
576.8 ms frame, showing that large stalls are not exclusive to either
candidate. Cleaner repeated measurements below support accepting only this
preflight change into the current branch; it is not yet released.

Three separate 90-second scheduler captures confirmed lower D3D12 worker CPU
occupancy with the shorter wait, but are excluded from throughput comparisons.
Two accurately aligned captures show some 25–40 ms frames waiting on the
diagnostic stderr stream lock. Exact call-site disassembly identifies the
stream, and the readying thread is the diagnostic periodic `fflush` worker.
Neither capture reproduced a frame above 50 ms. PowerShell redirected-pipe
backpressure is a hypothesis, not a proven cause of all stalls. A private
launcher using inherited disk handles isolates that measurement variable
without changing the game executables. Its results must be compared within
the same launcher configuration; a harness improvement is not a shipped game
optimization. The production Windows launcher already uses file handles.

Two direct-file baseline/candidate pairs retain the exact executables, isolated
saves, settings and normal-time race procedure described above:

| Run | Main CPU ms/frame | Process CPU ms/frame | FPS | P99 / maximum frame ms |
| --- | ---: | ---: | ---: | ---: |
| L baseline | 9.613 | 28.603 | 73.19 | 16.50 / 22.94 |
| M preflight | 9.218 | 27.613 | 76.19 | 15.89 / 29.31 |
| N baseline | 9.441 | 28.652 | 74.19 | 16.36 / 24.50 |
| O preflight | 9.187 | 28.054 | 74.92 | 15.97 / 24.25 |

Adjacent main-thread reductions are **4.10% and 2.70%**, exceeding the 1.78%
drift between these controls. Mean vertex/index counts differ by about
0.21%/0.23%, while draw count differs by 2.22%; the workloads are not identical.
All four middle windows have no frames above 33.33 ms. Candidate P99 improves,
but M has a worse maximum than both controls. Adjacent FPS gains vary from
0.99% to 4.10%; there is no claim of uniformly better worst-case latency or a
fixed FPS gain.

Independent scope, correctness and measurement reviews support the narrow
preflight change. The promoted source was freshly compiled against the four
standalone memory oracles and exactly matches the original baseline results.
This evidence covers one stationary Dolphin Resort scene on Ally X, not every
course or Android. The D3D12 worker budget remains unchanged.

A separate ordered special-word lookup passed correctness tests but is **not
accepted**: its main CPU/frame is 9.4565 ms between original controls at 9.4412
and 9.5359 ms, so no benefit beyond drift is established. Its 74.36 FPS also
falls too close to the 74.19/73.49 FPS controls to support integration. The
original lookup remains in production; simpler asymptotic complexity alone
does not demonstrate a gameplay gain.

### 2026-10-03: fresh Ally Vulkan attribution after memory preflight

A separate 35-second WPR capture on Ally X uses the accepted memory change,
Vulkan on Radeon 890M, and the existing bounded diagnostic race harness. It
retains full rendering and normal game/UI time, with direct file logging,
independent save/cache paths and detailed frame metrics enabled. This is CPU
attribution, **not** a new performance comparison or an API benchmark.

The exact executable SHA256 is
`35b701374f0f85f353f85e767c229bdc62bb6662d0b6723a047863c5b739a403`;
its linker-map SHA256 is
`2bd7ca62db52d1d7dffce79a09fad8a85562db45396e9a6a921a9bd54ae3b0b0`.
Extraction selects PID3540 and the first 35 seconds of the trace, resolving
the executable's recorded load base against this map. There are 44,384 process
samples and 22,640 main-thread samples (TID12512), with zero lost events/buffers.

| Main-thread path | Samples | Share of main CPU samples |
| --- | ---: | ---: |
| Indexed draw hook, inclusive | 7,288 | 32.19% |
| `native_draw`, inclusive | 5,574 | 24.62% |
| Indexed draw hook, leaf | 1,304 | 5.76% |
| Index decoding, leaf | 533 | 2.35% |
| Ordinary/special write preflight, leaf | 169 | 0.75% |
| AMD Vulkan driver module, leaf | 1,505 | 6.65% |

Inclusive categories overlap: `native_draw` is called by the indexed hook.
These percentages must not be added or treated as removable frame time. The
earlier D3D12 attribution was on a different machine and binary, so it does not
provide a controlled before/after CPU-sample comparison.

Exact instruction attribution places the hottest indexed-hook leaf samples in
the already-inlined fixed-stride vertex gather loops (not generic small-copy
calls). The renderer already uses GPU indexing for sufficiently compact index
ranges and a vertex cache for unchanged physical ranges. Sparse ranges and
primitive-restart conversion retain the gather path. The next useful probe is
therefore the **frequency, byte volume and write-epoch stability of sparse
gathers**, not another blanket memcpy replacement. Any reuse proposal must
account for index contents, base/restart semantics, vertex writes through all
mapped aliases, remapping and bounded cache ownership. No such cache or wider
upload threshold is introduced by this capture.

Other source checks found that index decoding already has SSE/NEON paths,
texture lookup still times every call, and draw-signature logging still probes
an ordered set on every draw. Their existence alone does not justify a claimed
FPS gain; timing costs and diagnostics should be measured separately from
required rendering work.

The game ended normally after 390.1 seconds. Inspected Dolphin Resort race
screenshots show HUD34.01 ->103.83 seconds, versus 69.809 logged seconds between
them. Source fixture hashes remain unchanged; the owned game, completed task,
WPR recording and temporary wake session were cleaned up. The power plan was
not changed. No Android test was started. This capture does not cover a full
lap, audio listening quality or other courses.

Evidence: `out/cpu-profile-v047/directlog-memory-vulkan-u`, including
`verification.json`, `analysis-provenance.json`, `attribution.txt` and
`indexed-draw-annotated.asm.txt`. ETL SHA256:
`335aa84b08cc7bf743b3afe9c1df342cc1c54e1fc44235542eb2b310ec437fb6`.
