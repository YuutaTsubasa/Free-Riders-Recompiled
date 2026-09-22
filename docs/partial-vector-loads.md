# 原始解碼流程越過向量部分讀取

**Windows 原始標題至主選單仍未達成。** 本次真實啟動已越過原先的
`824E46A0` 解碼函式停止點，原始流程繼續建立並啟動工作執行緒12至16，
最後停在 `XamNuiGetDeviceStatus`。尚無原遊戲 Draw／Present、標題或確認輸入。

## 改動與來源核對

為 `lvlx`／`lvrx` 及其128版本提供受檢查的部分向量讀取，保留原始
32-bit有效位址計算與向量位元組順序。只預先檢查、讀取指令選取的範圍；
對齊的右側讀取直接得到零，不存取記憶體。完整讀取成功後才更新目的向量，
失敗時保留其內容，不更動保留式原子操作狀態。

產生器僅接受固定版本 XenonRecomp 的完整原始指令展開，驗證左右遮罩共512個
位元組。修改運算元、位址寬度、遮罩、目的向量、條件分支或附加操作均會拒絕。
原始解碼函式的266條指令全部保留；六個部分讀取、四個部分儲存、scalar重疊
複製迴圈與尾端合併邏輯已逐一核對。沒有用主機解壓縮器替代原始程式。

本次生成共保留32028個函式；新增87個函式原本均只因未支援／未檢查的向量
記憶體操作被拒絕。580個新部分讀取的運算元與位址運算逐一核對，未移除其他
拒絕理由。這些數量只說明轉譯範圍，不能證明遊戲畫面或完整解碼資料正確。

## 真實執行證據與限制

`out/partial-load-boot.log` 記錄同一原生process：

- 從原始入口 `824D22F0` 啟動。
- 非同步讀入 FNT_SE 的14774 bytes，SHA-1仍為
  `a0002b1d9e8f7eadd3be7ce07779a1c4ddd80003`。
- 原始worker10在LR `824D417C`／`824D4190` 發出事件通知，繼續回到工作等待。
- 原始流程建立並啟動worker12至16；各工作在真正建立的native thread執行。
- `XamNuiGetDeviceStatus` 的呼叫位址為 `82ACB43C`，LR `824D0D84`，
  r3為 `70131770`；因該匯入尚未接通而停止，exit code為3。

解碼輸出的獨立雜湊／內容驗證仍未完成，不能僅由原始流程繼續而宣稱所有解碼
結果正確。下一項依賴是查明Kinect裝置狀態ABI及原始分支，再銜接已選定的
鍵盤／手把操作方向，不能虛構成功的感測器資料。

## 驗證與重現

55項CTest、192項Python測試通過，無略過。原生測試覆蓋16種偏移、獨立切片
順序、成對讀取、位址上界、選取範圍內外的guard／provider、失敗時目的保留、
對齊右讀取零存取與reservation。產生器測試包含四種指令及全部512個遮罩
位元組的個別變異。真實入口回歸測試先以舊執行檔重現824E46A0停止，再核對
新版本越過該點。既有執行緒數量斷言依實際新增的五個原始工作更新。

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-partial-loads
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

產生目的地必須不存在；本機已生成者直接使用後兩行。輸出保持在忽略目錄，
ROM及資產不提交。證據：`out/partial-load-generation-audit.json`、
`out/partial-load-retention-audit.json`、`out/partial-load-native-build.log`、
`out/partial-load-ctest.log`、`out/partial-load-python.log`；來源版本、執行檔與
同次啟動日誌雜湊記錄於 `out/partial-load-checkpoint.json`。
