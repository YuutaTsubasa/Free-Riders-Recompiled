# 原始非同步讀取完成與解壓縮入口

**Windows 原始標題至主選單仍未達成。** 真實入口已用 Windows OVERLAPPED
讀取 `game:\FNT_SE`，將實際的14774 bytes寫回遊戲記憶體。原始工作執行緒收到
讀取完成通知，辨識 XCTD 檔頭並喚醒呼叫者，再處理下一筆原始解壓縮工作。
目前停在原始CPU解碼函式 `824E46A0` 的六個未接通的向量部分載入。
尚無原遊戲 Draw／Present、標題画面或確認輸入。

## 實作與所有權

每筆請求保有真正的非同步檔案物件、獨立 OVERLAPPED、私人完成事件與對齊的
暫存記憶體。offset及長度依實際磁碟logical sector檢查，暫存位址依檔案的
alignment requirement檢查。原始guest buffer僅4-byte對齊，OS不直接寫入它。
實際 ReadFile／GetOverlappedResult 決定同步完成、pending、短讀、EOF及取消結果。
原生錯誤持續保持失敗，不能在再次查詢時變成零bytes成功。

提交前保留guest資料與IO status block範圍，禁止衝突寫入、decommit、release及
重新commit。完成工作取得同一個 GuestExecution 排程器的獨占執行權，檢查所有
輸出範圍，寫入實際資料與大端status／count，釋放request所有權後才通知保留的
guest read event。完成工作不冒充PPC執行緒，不事後修改原呼叫的r3。
這些檢查保護映射與寫入範圍；不能宣稱完整辨識guest heap僅更動外部metadata的
free／reuse。已觀測的原始讀檔路徑會等待該次完成通知後才繼續使用buffer。

初始guest事件重設與STATUS_PENDING寫入發生在排程器成功配置等待狀態之後，
仍持有執行權時。停止流程先拒絕新呼叫，等待提交中的呼叫完整退出，再取消、
drain並join所有請求，最後釋放暫存、事件與guest範圍。即使原檔案／event registry
已銷毀，保留的native物件仍有明確所有權。

只接受本次原始路徑所需的event、顯式offset及零APC routine。
`STATUS_PENDING (0x103)` 轉為 `ERROR_IO_PENDING (997)`；原始呼叫者自行處理
pending分支，不把排隊成功當成資料完成。

## 同次真實執行

`out/pending-status-boot.log` 的同一process留下：

```text
RESULT NtReadFile handle=0x72000008 buffer=0x401a3834 requested=131072 offset=0 transferred=0 status=0x103
NATIVE_ASYNC_FILE_DATA handle=0x72000008 event=0x72100034 io=0x82b50114 buffer=0x401a3834 offset=0 requested=131072 transferred=14774 status=0x0 sha1=a0002b1d9e8f7eadd3be7ce07779a1c4ddd80003
RESULT NtWaitForSingleObjectEx guest_id=10 handle=0x72100034 status=0x0 milliseconds=0 infinite=1 backend=windows-sync
RESULT NtSetEvent guest_id=10 handle=0x72100044 previous_output=0x0 status=0x0 lr=0x824d433c backend=windows-event
RESULT NtWaitForSingleObjectEx guest_id=3 handle=0x72100044 status=0x0 milliseconds=0 infinite=1 backend=windows-sync
IMPORT RtlNtStatusToDosError status=0x103 result=997
STOP worker-unsupported-function @0x824e46a0: sub_824E46A0: unsupported_vector_memory; unchecked_guest_memory [guest_id=10 function=__restgprlr_18 address=0x82a56090 LR=0x824e4b30]
```

資料SHA-1與已核對ISO來源的原資產一致。原始worker10進入 `824DDCF8`，證明
`824D3850` 已接受magic `0x0FF512ED` 與cache-size條件。隨後worker4經
`824DF010 → 824D2D08` 排入type2工作，worker10沿原始 `824D3B10` 壓縮資料
分支進入解碼。原始decoder的完成、輸出內容及下一筆請求完成均尚未證實。
兩個guest事件用途不同：host只通知read event；檔頭完成事件由原始遊戲程式通知。

## 驗證與重現

55項CTest、188項Python測試通過，無略過。新增測試涵蓋真實pending／短讀／EOF、
cancel與完成競爭、LockFileEx產生的實際錯誤、檔案／事件保留、guest輸出範圍與
原子操作衝突、排程配置失敗及停止時提交尚未退出的生命週期。測試先重現缺失或
失敗，再核對修正；pending由真正OS結果決定，沒有強制pending實作。
真實入口測試核對資料雜湊及「資料發佈 → read wait完成 → 原始檔頭signal →
呼叫者wait完成」，並允許OS實際同步完成的合法結果。

完整測試：`out/native-async-read-ctest.log`、`out/native-async-read-python.log`。
關鍵RED：`out/native-async-primitives-red.log`、`out/native-async-error-red.log`、
`out/async-shutdown-red.log`、`out/async-admission-red.log`、`out/pending-status-red.log`。
獨立原始碼與生命週期審查：`out/async-memory-lifetime-audit.md`、
`out/original-async-read-next-audit.md`及其JSON原始指令／雜湊紀錄。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-format
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

目前明確停止時exit code為3。來源commit、執行檔SHA-256、測試與同次日誌索引
見 `out/native-async-read-checkpoint.json`。ROM、資產、生成碼及執行產物不提交。
[前一份非同步開啟／佇列紀錄](async-file-startup.md)保留為歷史證據。
