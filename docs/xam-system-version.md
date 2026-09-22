# 原始系統版本與具名 XAM 查詢

本頁保留此步驟的歷史紀錄；後續已通過原始Winsock回退路徑，見 [目前證據](native-winsock.md)。

**標題至主選單仍未達成。** 真實原生啟動已完成系統版本查詢，依遊戲原始
較新版本分支查找 `xam.xex`，取得現有原生模組識別，再要求動態匯出
`NetDll_WSAStartupEx`。目前停在該匯出尚未接通的位置。

版本設定為明確的相容性目標 `0x20308000`（2.0.12416.0），與已驗證本機
XEX的XAM及kernel匯入version／minimum_version一致。這不是Windows版本，
也不表示所有Xbox介面都已實作。沒有回傳較低版本來避開原始動態查詢分支。
固定版本Xenia對應函式回傳0且標示stub，未採用該政策。

具名查詢只接受目前確認的 `xam.xex`，回傳已存在的guarded模組識別
`71500000`。完整擷取名稱後才写入BE32結果，輸出成功後才允許後續借用識別
查詢；不增加load count。null名稱仍回到既有原始執行模組查詢。
未知名稱及未實作匯出仍會停止，沒有宣告不存在的功能已成功。

同一次 `out/xam-query-boot.log`：

```text
XAM_SYSTEM_VERSION value=0x20308000 target=2.0.12416.0 source=verified-game-abi
RESULT XexGetModuleHandle name=0x82000910 output=0x70131574 status=0x0 module=0x71500000 retained=0
STOP native-module-export @0x24: only queried or acquired XAM known optional exports are recognized
```

原始 `8270CDF8` 比較版本後查找XAM ordinal36；固定版本
[XAM匯出表](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_table.inc)
將它命名為 `NetDll_WSAStartupEx`。查詢LR為 `8270CE54`。這是接下來需要
處理的原生網路函式庫初始化依賴，尚未開啟socket或建立網路連線。

56项CTest、192项Python測試通過。新增回歸先捕捉缺少版本查詢與具名模組
查詢的失敗，再核對真實原始分支；模組測試涵蓋借用身份、load count、
名稱／輸出別名、失敗輸出及失敗後不得啟用身份。獨立審查未發現功能缺陷；
指出的過時錯誤文字已修正並重新建置、實跑。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

實跑exit code為3；尚無原始Draw／Present、標題或確認輸入。證據：
`out/xam-query-build.log`、`out/xam-query-ctest.log`、`out/xam-query-python.log`、
`out/xam-query-boot.log`；最終來源、執行檔與日誌雜湊在
`out/xam-query-checkpoint.json`。本次沿用先前核對的生成碼，未更改原遊戲分支。
