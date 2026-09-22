# 原始 alpha、depth 與 cull 設定

Windows 真實入口已從原始 `827F5F40` 完成七項renderer state呼叫，接續到
`824F4220/LR827F5FC8` 的slot0貼圖綁定，實際參數為null及mask80000000。
原生owner持有各項已設定的typed state；尚未由遊戲draw／Present消費，沒有
標題畫面或主選單，驗收目標仍未達成。

## 已接通的原始語意

| 函式 | 狀態 | 真實請求 | Guest cache與dirty mask（十六進位） |
| --- | --- | --- | --- |
| 824E6A08 | alpha test enable | 1 | 293C bit3；40200 |
| 824E6EC8 | alpha comparison | GREATER（4） | 293C bits0..2；200 |
| 824E6E68 | alpha reference | 0 | 2904 float；8000000 |
| 824E70D0 | depth requested enable | 1 | 2F14原值、2934 bit1依附件；20800 |
| 824E7140 | depth comparison | LESS_EQUAL（3） | 2934 bits4..6；20800 |
| 824E7110 | depth write | 1 | 2934 bit2；800 |
| 824E69A8 | cull/winding | BACK、CW（6） | 2948 bits0..2；40 |

Cache皆為device相對offset；dirty全部OR進device+10的64-bit word，保留既有
bits。同值重設也標記dirty。原始呼叫者及其迴圈不變，七個strong entry hooks
依精確函式入口與完整ABI參數契約辨識，已移除僅限初始化LR的限制；沒有
供應成功結果碼。後續depth comparison6及cull呼叫的證據見
[玩家初始化之後的原始設定](user-initialization.md)。其他native-object consumers保持保護。

`NativeRenderState` 以optional欄位保存alpha enable/function/reference、depth
request/function/write以及cull/winding。未執行的原始設定保持未知，不填入猜測
預設。所有輸入、來源讀取及完整guest寫入preflight成功後，才發布guest cache、
dirty及native state；拒絕不留下部分更新。

Depth enable保存原始request。有效值由request與目前已知depth attachment共同
決定，讀取時重新計算，避免detach後仍沿用先前true；未知non-null attachment
明確停止。後續真正attachment切換還必須維持原始cache與原生資源狀態。

Alpha reference核對原始82001658的float bits3B808081，依原始fcfid/frsp/fmuls
使用0..255輸入及單精度結果；並非改用double除255的近似流程。原生實作以strict
FP編譯，hook保留原始disableFlushMode。比較函式支持全部8個Xenos值，bool僅
接受0/1，cull支持0/1/2/4/5/6；非法值、未知enum及變更／缺少常數均明確停止。

## 同次執行與回歸

`out/renderer-states-boot.log`（退出碼3）記錄全部七次 `NATIVE_RENDER_STATE`，
以及以下實際新邊界：

```text
NATIVE_GRAPHICS_BOUNDARY address=0x824f4220 lr=0x827f5fc8 r3=0x71600000 r4=0x0 r5=0x0 r6=0x80000000 r7=0x83e60000 r8=0x82000000 r9=0x82b60000
STOP native-graphics-function @0x824f4220: original function cannot consume an unimplemented native object layout
LAST_FUNCTION sub_824F4220 @0x824f4220 LR=0x827f5fc8 calls=13408
```

同次執行仍包括原始21次貼圖建立、矩陣複製、caps／呈現參數核對及混色狀態。
呼叫總數受原生執行緒排程影響，並非固定驗收值。來源繼續使用
`out/recomp/diagnostic-matrix-copy`：

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-matrix-copy
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

完整建置 `out/renderer-states-green-build.log`；40項原生測試及156項Python
測試通過、無略過，見 `out/renderer-states-ctest.log`、
`out/renderer-states-python.log`。新元件先以可編譯stub取得RED，再取得GREEN，
記錄於 `out/renderer-states-red.log`、`out/renderer-states-green.log`；真實入口
RED在 `out/renderer-states-entry-red.log`。測試逐byte核對整個device，覆蓋全部
合法比較／bool／cull、256個reference、同值dirty、動態附件、late readonly/import
guard與live atomic的拒絕原子性。完整語意與整合已經獨立唯讀審查。
Commit、執行檔hash及證據索引保存於 `out/renderer-states-checkpoint.json`。

## 尚需接通

下一步為原始八slot貼圖綁定迴圈。當前slot0確實為null；其餘全域變數需在原始
執行時讀取，不能以映像初始值代替動態值。Null設定會清除fetch dword0的type
bits並保存null binding；舊binding仍涉及fence／deferred retirement，nonnull
還需要真正的texture view、sampler及shader-read資源狀態。詳見
`out/texture-bind-next-audit.md/.json`。

Alpha test沒有D3D12固定功能對應，這輪保存的狀態仍須接入實際pixel shader。
已發現現有XenosRecomp產物的 `clip(alpha-threshold)` 不會丟棄相等值，不能
直接等同這次原始GREATER比較；後續必須修正其比較語意與特殊化接口，不能
把「state已保存」或「library已存在」當作畫面完成。
