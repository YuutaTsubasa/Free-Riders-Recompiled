# 原始非同步資產開啟與讀取佇列

**標題至主選單目標仍未達成。** Windows 原生執行檔已沿原始入口開啟
`game:\FNT_SE`，通過 Xbox XCTD 資訊不可用時的原始分支，喚醒原始讀檔工作
執行緒。它現在提出真正的非同步讀取請求，尚未完成資料傳輸。
尚無原遊戲 Draw／Present、標題畫面或確認輸入。

## 已驗證的行為

原始檔案開啟要求為唯讀 access `80120089`、share1、disposition1、options48。
相容層使用 Windows `FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING`，保留實際
原生檔案物件、分享限制、mount 邊界及關閉所有權。既有同步檔案路徑保持正常；
對非同步物件呼叫尚未支援的同步讀取會明確停止。

同次實跑透過實際 NT 查詢確認 handle `72000008` 的 access=`120089`、mode=`8`，
檔案系統 alignment requirement=`3`。這個 mask 表示4-byte位址對齊需求，
不代表 sector 大小或 physical sector 對齊。

ROM、擷取檔與 manifest 的 FNT_SE 均為14774 bytes，SHA-256：
`ded1d6fedcce7567cfd16c6f24b5c0e34fbb405b239b9ec512cb0e95518323f3`。
原始前16 bytes 為 `0ff512ed010000005986c63a00000060`。
`0FF512ED` 是原始程式比較的 XCTD 標記；沒有據此捏造成功的壓縮屬性查詢。

Xbox class27 的4-byte查詢回報明確不可用狀態 `C000000D`，IO資訊長度0，
與 [固定版本相容層參考](https://raw.githubusercontent.com/xenia-project/xenia/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc)
一致。它不是 Windows 的同編號資訊類別，也沒有傳回「檔案未壓縮」成功結果。
原始 `824DE7A0` 自行處理失敗，經 `824D2B68` 排隊讀取檔頭。

原始 producer 呼叫 `NtSetEvent`，經真正 Windows event 喚醒 worker10。
目前支援本次觀測到的 null previous-state output；要求先前狀態時在變更事件前
明確停止，沒有捏造狀態。呼叫者3接著等待自己的完成事件，worker10則進入原始
`824D3850 → 824DD248 → 824DD178` 讀取鏈。

## 同次真實執行證據

`out/file-queue-event-boot.log` 保留完整紀錄，以下為其中片段：

```text
RESULT NtCreateFile status=0x0 handle=0x72000008 size=14774 path=game:\FNT_SE
NATIVE_FILE_MODE handle=0x72000008 access=0x120089 mode=0x8 alignment_requirement=0x3 backend=windows-file
RESULT NtQueryInformationFile handle=0x72000008 class=27 length=4 status=0xc000000d
RESULT NtSetEvent guest_id=3 handle=0x7210002c previous_output=0x0 status=0x0 lr=0x824dd700 backend=windows-event
RESULT NtWaitForSingleObjectEx guest_id=10 handle=0x7210002c status=0x0 milliseconds=0 infinite=1 backend=windows-sync
FILE_READ_ABI guest_id=10 lr=0x824dd208 handle=0x72000008 event=0x72100034 apc=0x0 context=0x82b50114 io=0x82b50114 buffer=0x401a3834 length=0x20000 offset_pointer=0x74060c50
STOP worker-file-read @0x72000008: event and APC file reads are unimplemented [guest_id=10 function=__savegprlr_29 address=0x82a5606c LR=0x824dd208]
```

要求長度是128 KiB，檔案大小14774 bytes，buffer僅4-byte對齊。APC routine為0，
context與IO status block同址。沒有預先讀取 offset 指標來改變服務檢查順序；
原始組碼顯示初始chunk為0，仍須在接受讀取時核對真正的 offset 值。

下一步是實際 OVERLAPPED 讀取、完成通知與取消。Windows 完成事件必須與guest
事件分開，原生完成後需取得 GuestExecution 執行權，依序發佈資料、BE IO status／
byte count，再 signal guest event；pending buffer、file、event 的生命週期及
關閉時 cancel/drain 都必須正確。原始遊戲將自行解析實際傳回的檔頭。

## 驗證與重現

51項CTest、188項Python測試全部通過，無略過。新測試先在缺失實作下失敗，
再核對真正的Windows模式、唯讀權限、分享衝突、junction mount邊界、關閉釋放、
guest大端輸出、XCTD失敗及各種無副作用拒絕。真實入口測試核對
「原始signal → 真正wait成功 → 原始read要求」順序；具名事件使用同次建立的handle，
避免把不同工作執行緒的配置先後當成固定編號。

測試紀錄：`out/async-file-startup-ctest.log`、`out/async-file-startup-python.log`。
RED紀錄：`out/async-open-red.log`、`out/xctd-query-red.log`、`out/file-queue-event-red.log`。
API／所有權與guest整合均已獨立審查；審查指出的offset預讀已移除。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-format
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

執行檔、來源commit、命令、SHA-256及同次證據索引在
`out/async-file-startup-checkpoint.json`。ROM／資產、生成碼及執行產物不提交。
前一份 [初始化紀錄](startup-continuation.md) 保留歷史證據。
