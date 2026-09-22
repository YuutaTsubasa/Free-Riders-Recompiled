# 原始網路函式庫初始化

本頁保留Winsock步驟的歷史紀錄；目前已繼續至 [通知監聽器與登入查詢](native-notifications.md)。

**標題至主選單仍未達成。** 原始遊戲已透過自己的相容性分支呼叫真正的
Windows Winsock 初始化，成功後繼續至 `XamNotifyCreateListener`。
本次沒有替換原始 wrapper，也沒有產生或顯示替代畫面。

原始 `8270CDF8` 先按既有系統版本查詢 XAM ordinal36（`WSAStartupEx`）。
此原生 runtime 尚未提供該 SDK 擴充，回傳既有 missing-export 狀態
`C0000263` 與 null procedure。原始指令自行還原三個參數，再於 `8270CE98`
呼叫 plain `NetDll_WSAStartup`，回傳位置為 `8270CE9C`。未知的其他匯出仍
停止。Ex 第四參數 `202DFF03` 與原始 XNET 靜態庫 2.0.11775.3 相符，
但其行為契約尚未確認，因此不宣稱已實作 Ex。

plain import 綁定真正 `WSAStartup`，回傳實際錯誤或協商結果。原始
`8278DCF8` 的496-byte stack frame從SP+80提供400 bytes，SP+480即為
保存的r31；Windows 64位元 `WSADATA` 的欄位排列不同，必須逐欄轉換。
輸出含BE16版本、257-byte描述、129-byte狀態及BE16舊版數值欄位，
vendor欄位位於396，不能把主機指標放入guest。Winsock2忽略該欄位，
本實作填null；未提供的1.x vendor資料轉換會清理初始化資源後明確停止。
完整400-byte寫入範圍在主機初始化前檢查，每次成功取得資源均有對應
`WSACleanup`；程式結束時先停止遊戲工作執行緒，再清理剩餘取得次數。

同一次 `out/winsock-boot.log`：

```text
UNAVAILABLE XexGetProcedureAddress module=xam.xex ordinal=0x24 output=0x70131570 result=0x0 status=0xc0000263
NATIVE_WSA_STARTUP caller=1 requested=0x2 output=0x701315f0 bytes=400 status=0x0 version=0x2 high_version=0x202 acquisitions=1 lr=0x8270ce9c backend=windows-winsock
IMPORT_CONTEXT name=__imp__XamNotifyCreateListener address=0x82acb40c lr=0x82232e3c sp=0x70131790 r3=0x1 r4=0x6
STOP import-function @0x82acb40c: __imp__XamNotifyCreateListener
```

上方IMPORT_CONTEXT只摘錄到r4；完整日誌保留其餘暫存器。實跑exit code3。
初始化成功不表示已有網路連線、帳號或通知事件。尚無原始Draw／Present、
標題或鍵盤／手把確認。

57項CTest、192項Python測試通過，無略過。先驗證缺少實作時失敗，再驗證
實際Windows協商資料、精確400-byte輸出、範圍錯誤前置拒絕、原生錯誤、
多次取得、明確清理及解構清理。測試建立本機socket檢查初始化生命週期，
不連線或傳送資料。原始入口回歸另核對選用fallback及正確LR。
獨立程式審查未發現此範圍內的功能缺陷。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

證據：`out/winsock-build.log`、`out/winsock-ctest.log`、`out/winsock-python.log`、
`out/winsock-boot.log`。原始43個指令與XNET版本核對在
`out/wsa-startup-ex-audit.md`；來源、執行檔及日誌雜湊在
`out/winsock-checkpoint.json`。

參考：[Microsoft WSAStartup](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsastartup)、
[WSADATA](https://learn.microsoft.com/en-us/windows/win32/api/winsock/ns-winsock-wsadata)、
[WSACleanup](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsacleanup)，
以及固定版本[Xenia XAM網路介面](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_net.cc)。
未採用其非Windows猜測資料、no-op cleanup或超出本遊戲buffer的offset400存取。
