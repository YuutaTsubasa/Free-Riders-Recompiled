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


