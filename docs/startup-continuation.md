# 原始啟動延續：國家轉換、工作執行緒跳轉與鎖等待

**標題至主選單目標仍未達成。** 最新真實入口已完成原始國家轉換，返回34，
並繼續初始化；工作執行緒接著提出尚未支援的檔案開啟要求。尚無遊戲 Draw／Present、
標題畫面或確認輸入。

## 已驗證的改變

- Windows `GetUserDefaultGeoName` 實際回報TW，相容層映射為Xbox country101；
  原始category3/setting14查詢精確寫入BYTE國家及BE16長度。來源獨立於介面語言
  與明確指定的NTSC-US執行設定，詳見 [國家設定](user-country.md)。
- 原始 `0x824D1BA8` 的表格跳轉恢復為函式內分支，保留105-byte表、38個目的地、
  128條指令與原始frame。實跑在下一個原始函式入口觀測到返回34。
- 工作執行緒格式化函式 `0x82A68220` 保留112-byte BE16表、15個函式內目的地、
  722條指令、1328-byte frame、既有lhzu轉譯，以及3個真正的linked間接呼叫。
  只修正plain bctr；完整原始body hash限制適用範圍，未知目的地仍停止。
- 臨界區使用原生執行緒等待與喚醒。等待時釋放GuestExecution執行權，取得原始
  擁有者的喚醒後再恢復執行權、完成guest ownership；保留遞迴與waiter計數，
  防止插隊及遺失喚醒。全域取消不回報成功。初始化可接受任意未追蹤儲存空間，
  已有owner／waiters／handoff則不可重新初始化。

臨界區基於 [固定版本Xenia RTL語意](https://raw.githubusercontent.com/xenia-project/xenia/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc)。
guest物件維持28-byte原始布局；純host等待不讀寫guest記憶體，guest修改均在執行權內。
配置失敗不留下等待者；丟棄未完成token會終止coordinator並喚醒同伴。
有符號lock-count達INT32_MAX時拒絕再遞增。保留不受影響的header／list／spin bytes。

## 真實執行

```text
NATIVE_USER_COUNTRY source=GetUserDefaultGeoName iso=TW xbox_country=101
ORIGINAL_COUNTRY_TRANSLATION_RETURN value=34 next=0x824d0b58 lr=0x82439678
...
REQUEST NtCreateEvent output=0x732249d0 attributes=0x0 type=0x0 initial=0x0 lr=0x824d01fc
RESULT NtCreateEvent output=0x732249d0 handle=0x7210003c status=0x0 type=0 initial=0 backend=windows-event
IMPORT RtlInitAnsiString destination=0x732249b8 source=0xff6fefc8 length=12 maximum_length=13
FILE_OPEN_CONTEXT root=0xfffffffd name=0x732249b8 attributes=0x40 options=0x48
STOP worker-file-open-request @0x732249c8: only synchronous read-only existing-file open is implemented [guest_id=3 function=__savegprlr_17 address=0x82a5603c LR=0x824de908]
LAST_FUNCTION sub_82A561E0 @0x82a561e0 LR=0x822202bc calls=19937
```

完整同次日誌：`out/startup-continuation-boot.log`。工作執行緒排程會影響最後的主執行緒
呼叫位置；這些是觀測紀錄，不是修改或跳過原始路徑的條件。12-byte路徑的實際文字
尚未由這份日誌觀測，不能先假定它是某個資產或log。

## 驗證與重現

50項CTest與188項Python測試全部通過，無略過，分別保存在
`out/startup-continuation-ctest.log`、`out/startup-continuation-python.log`。
原始國家函式另有隔離測試2048種國家／查詢狀態／執行區域組合及未知CTR拒絕，
核對結果、stack、LR和r14。這是語意單元測試，不是替代遊戲啟動。

臨界區新增14組測試，包含真實執行緒handoff、取消與stop/drain、配置失敗逐點注入、
同coordinator恢復使用及計數上限。保留修正前物件檔執行新回歸，先報上限未拒絕，
再以`0xC0000409`異常退出；修正後完整測試正常完成。失敗證據在
`out/critical-contention-regression-red.log`，未以重跑掩蓋。

相較先前診斷來源，只有國家與格式化這兩個原始函式的生成碼改變；原始檔、診斷log、
rejected functions及函式映射表不變。沒有加入interior function mappings。
兩份核對結果在 `out/user-country-generation-audit.json` 與
`out/format-ctr-generation-audit.json`。原始shader仍是74個已核對資源，
其中37個pixel library尚未完成實際GPU消費。

在已有本機映像、資產、shader cache與新診斷來源的此工作目錄：

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-format
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

需要重新生成時使用尚不存在的輸出目錄：

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-format-next
```

執行檔、來源commit、SHA-256、命令與同次證據索引保存在
`out/startup-continuation-checkpoint.json`。ROM、資產、原始生成碼與測試輸出均不提交。
