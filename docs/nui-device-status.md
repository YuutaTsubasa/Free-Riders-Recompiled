# 原始 Kinect 裝置狀態查詢

**標題至主選單目標仍未達成。** 原始流程已連續完成兩次
`XamNuiGetDeviceStatus` 查詢，取得目前沒有 Kinect backend 的狀態，
接著停在 `XamGetSystemVersion` 系統版本查詢。

原遊戲 `824D0D70` 的wrapper配置24-byte結果，從offset12讀取status。
固定版本Xenia的公開介面同樣定義六個BE32欄位、void回傳及status0表示未連接。
該上游介面標示kStub，只能作為裝置缺席相容設定的依據，不能當成完整硬體協定。
來源見 [固定版本NUI介面](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_nui.cc)。

本次實作在任何寫入前檢查完整24-byte範圍，再透過既有受檢查記憶體操作寫入
缺席狀態。匯入以精確名稱與位址綁定，保留void語意；未宣告連接的感測器、
未產生骨架資料，也未修改遊戲的原始判斷分支。鍵盤／手把輸入適配仍待完成。

`out/nui-status-boot.log` 的同一原生process留下兩筆：

```text
NUI_DEVICE_STATUS output=0x70131770 bytes=24 status=0x0 backend=absent-kinect
STOP import-function @0x82acb35c: __imp__XamGetSystemVersion
```

下一項查詢的LR為 `8270CE1C`，原始程式用回傳版本決定是否尋找動態匯出。
不能任意回傳低版本來迴避尚未實作的依賴。已驗證XEX的XAM與kernel匯入
version及minimum_version均為 `0x20308000`（2.0.12416.0），可供下一項
明確相容性目標的設計依據；目前尚未實作系統版本查詢。

56項CTest、192項Python測試通過，無略過。新增測試先重現缺少實作的失敗，
再驗證精確寫入範圍、鄰接位元組、地址上界及溢位、import／read-only／pending
衝突下的整段寫入前檢查。真實入口測試確認狀態查詢已發生。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

本次使用前次已驗證的生成來源，不重新改寫原始遊戲。實跑exit code為3；
尚無Draw／Present、原始標題或確認輸入。證據在 `out/nui-status-build.log`、
`out/nui-status-ctest.log`、`out/nui-status-python.log`、`out/nui-status-boot.log`；
來源版本、執行檔與日誌雜湊見 `out/nui-status-checkpoint.json`。
