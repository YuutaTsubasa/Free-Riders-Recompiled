# 原始通知監聽器

本頁保留通知步驟的歷史紀錄；後續已完成 [四個玩家欄位初始化](user-initialization.md)。

**標題至主選單仍未達成。** 真實遊戲流程已建立通知監聽器、保存回傳的
handle，並要求通知顯示於下方中央，接著停於 `XamUserGetSigninState`。
監聽器具備真正的 Windows event 與通知佇列；沒有注入猜測的開機通知。

原始 `824D0D20` 保留64位元分類mask，設定最大版本6，再呼叫
`XamNotifyCreateListener`。目前請求mask為1（system），回傳的32位元
guest handle存入原始物件+`7EE0`；它不是Windows指標。監聽器的
manual-reset event起初未發出訊號，符合分類與版本的項目入列後才發出
訊號，最後一個項目取出時重設。普通event set/reset及非同步檔案事件
capability均不能修改此類事件。

`XNotifyGetNext`支援依序或指定ID取出，先檢查完整guest輸出範圍，再於
鎖內寫入並取出。查無符合項目時清除指定輸出，但其他待讀項目及訊號
保持不變。參數輸出可為null；兩輸出重疊時依ID、參數順序寫入。
`NtClose`先辨識監聽器，再移除佇列與guest handle；已取得的原生等待
handle仍有獨立生命週期。每個監聽器最多4096項、最多1024個監聽器，
超限明確停止。建立、讀取、關閉及身分查詢須持有guest執行權；
`publish`可由原生producer執行，僅接觸受鎖保護的佇列及已保留事件。
結束前須先停止producer與遊戲工作執行緒。

位置設定保留遊戲要求的水平／垂直對齊，ABI為void，不改寫r3為成功碼。
原始請求2為下方中央，稍後其他原始呼叫使用5（左上方）。只有九種已核對
組合受支援；衝突或未知旗標不覆蓋前值。設定位置本身不建立彈窗、不移動
主遊戲視窗，也不產生UI通知。第一個設定前保持未指定。

同一次 `out/notifications-boot.log`：

```text
NATIVE_NOTIFICATION_CREATE mask=0x1 maximum_version=6 handle=0x7210005c queued=0 lr=0x82232e3c backend=windows-event
NATIVE_NOTIFICATION_POSITION flags=0x2 horizontal=center vertical=bottom lr=0x82232e48 state=retained-placement
ORIGINAL_NOTIFICATION_HANDLE owner=0x70137d40 offset=0x7ee0 handle=0x7210005c
STOP import-function @0x82acb32c: __imp__XamUserGetSigninState
```

下一個查詢的原始LR為 `822344D8`，唯一參數user index為0。實跑exit3。
這次只驗證原始建立、保存及位置設定；原始輪詢與關閉尚未實際到達。
佇列的入列、取出、等待與關閉由獨立測試驗證，不能當作遊戲已收到通知。
真正的輸入裝置、離線使用者或UI轉換producer尚未接入；不能將Windows
帳號登入視為Xbox登入，也沒有宣告Kinect或Xbox Live已連線。

60項CTest、192項Python測試通過，無略過。新增測試先捕捉未實作的失敗，
再核對真正event等待／訊號、mask高位與版本篩選、FIFO／ID匹配、輸出
guard不遺失項目、範圍上限、錯誤類型、關閉及保留等待生命週期，以及
由另一原生執行緒入列、解構後handle數量恢復。位置測試涵蓋原始使用的
2／5、明確0設定及錯誤旗標保留前值。獨立審查未發現阻擋性缺陷。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

日誌：`out/notifications-build.log`、`out/notifications-ctest.log`、
`out/notifications-python.log`、`out/notifications-boot.log`。來源與執行檔
雜湊：`out/notifications-checkpoint.json`。原始指令及來源核對：
`out/notify-listener-next-audit.md`、`out/notify-position-audit.md`。

介面及佇列參考固定版本[Xenia XAM通知介面](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_notify.cc)及
[通知監聽器](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xnotifylistener.cc)。
位置數值另核對[Canary固定版本的實際位置表](https://github.com/xenia-canary/xenia-canary/blob/5d4dc8a88abb2965f2933286571f5bfa0b87391d/src/xenia/ui/imgui_notification.h#L38)。
這些是公開實作參考，並非所有Xbox行為的官方規格；未採用Xenia猜測的
UI／登入開機通知，也未將其no-op位置stub視為渲染證據。
