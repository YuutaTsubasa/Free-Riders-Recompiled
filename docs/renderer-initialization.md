# 原始渲染器初始化與矩陣複製

Windows 真實入口已從原始21次貼圖建立，繼續執行渲染器初始化
`827F61A0` 的 CPU 設定。原始64-byte矩陣複製已返回，目的內容及來源不變均已
核對；304-byte能力資料與124-byte呈現參數由原始程式複製，遊戲自行選定
vertex/pixel shader等級3。尚未完成整個渲染器初始化，仍無原遊戲draw、Present、
標題畫面或確認輸入。

## 原始流程與適配範圍

只允許已核對的 `827F61A0/LR82224440` 接收原生device71600000，以及其
stack-save helper `82A56060/LR827F61A8`、靜態caps forwarder
`82504FA8/LR827F61D0`。這些函式本身不解讀原生裝置內部；後續真正消費
裝置狀態的函式保持獨立保護。沒有替換整個初始化函式或提供成功回傳值。

第一輪實跑先停於原始copy `824D1858/LR827F63BC`。其189條指令中的兩個
partial vector store及兩個128-byte `dcbzl` 未受檢查，因此整個函式遭拒絕。
現在嚴格核對完整標準發射區塊後，以保留有效位址及位元序的helpers執行：

- `stvlx/stvlx128`：從EA開始存放16−(EA&15)個前端向量bytes。
- `stvrx/stvrx128`：在EA之前存放EA&15個尾端bytes；對齊時為真正零存取。
- `dcbzl`：先完整檢查，再清零EA所屬的128-byte對齊範圍。

實作沿用既有反向向量byte表示、記憶體guard、原子保留限制及write-combine
同步。普通 `dcbz` 的32-byte語意不變。產生器拒絕變體、運算元不符、額外
敘述與不完整區塊，保留原始分支及指令註解。原始copy完整重編譯執行，沒有
使用native memcpy替換。這次實際64-byte對齊路徑使用既有完整vector操作，
**未執行該函式中的partial stores或dcbzl**；新指令語意另由單元測試驗證。

新來源 `out/recomp/diagnostic-matrix-copy` 保留31,726函式、拒絕14,777；
比前版新增37、沒有新增拒絕。保留函式內有548處partial stores及69處128-byte
清零。原始輸入與重編譯日誌雜湊不變，見 `out/matrix-generation-comparison.json`。

## 同次執行證據

`out/matrix-copy-boot.log`（退出碼3）記錄：

```text
ORIGINAL_RENDERER_INIT_REQUEST device=0x71600000 parameters=0x83e53ca8 vertex_selector=4 pixel_selector=4
ORIGINAL_RENDERER_MATRIX_COPY_REQUEST destination=0x82b628d0 source=0x701316e0 sp=0x70131680 bytes=64 sha1=8128ff7f6f6cbc14fc6fdb67dd8c745641cd243f
ORIGINAL_RENDERER_MATRIX_COPY_RETURN destination=0x82b628d0 bytes=64 match=1 source_unchanged=1
ORIGINAL_RENDERER_CPU_SETUP device=0x71600000 caps_bytes=304 caps_match=1 parameters_bytes=124 parameters_match=1 width=1280 height=720 vertex_selector=3 pixel_selector=3
NATIVE_GRAPHICS_BOUNDARY address=0x824e9218 lr=0x827f5f68 r3=0x71600000 r4=0x0 r5=0x7010706 r6=0x83e60000 r7=0x83e60000 r8=0x82000000 r9=0x82b60000
STOP native-graphics-function @0x824e9218: original function cannot consume an unimplemented native object layout
LAST_FUNCTION sub_824E9218 @0x824e9218 LR=0x827f5f68 calls=13407
```

觀察點 `8280C350/LR827F63DC` 位於copy返回後的下一次原始callee entry，
核對相同SP及來源指標。觀察器只讀記憶體，不修改guest結果或暫存器。
多執行緒呼叫總數可能隨排程改變，驗收以具體流程與資料為準。

下一個 `824E9218` 為per-target混色狀態setter；原始值07010706代表RGB
SRC_ALPHA／INV_SRC_ALPHA／ADD與alpha ONE／INV_SRC_ALPHA／ADD。需將其
語意保留於原生狀態並在後續PSO使用，不能只跳過呼叫。稽核記錄在
`out/state-824e9218-audit.md/.json`，其餘原始初始化與copy稽核分別在
`out/graphics-827f61a0-next-audit.md/.json`、`out/matrix-copy-next-audit.md/.json`。

## 重現與驗證

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-matrix-copy
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-matrix-copy
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

產生器輸出需為新目錄；現有目錄已生成，不應覆蓋。
本輪完整建置 `out/matrix-copy-build.log`，helper RED/GREEN記錄
`out/matrix-memory-red.log`、`out/matrix-memory-green.log`，產生器76項
測試 `out/matrix-generator-green.log`。新測試涵蓋全部對齊offset、有效邊界、
late guard無部分寫入、原子保留及Windows原生write-combine屬性。
完整回歸38項原生測試及156項Python測試通過、無略過，分別記錄於
`out/matrix-copy-ctest.log`、`out/matrix-copy-python.log`；執行檔雜湊保存於 `out/matrix-copy-checkpoint.json`。
helpers／嚴格產生器與原始forwarding／觀察點均已經獨立唯讀審查。
