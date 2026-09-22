# 原始混色狀態接入原生管線狀態

Windows 真實入口已通過原始 `824E9218/LR827F5F68` 混色設定，回到原始
`827F5F40` 呼叫者，進入下一個alpha test設定 `824E6A08`。混色資料由
原生裝置owner持有，供後續PSO建立使用；本輪沒有遊戲draw或Present，標題至
主選單目標仍未達成。

## 原始設定與原生語意

原始函式接收device、render target index及32-bit Xenos RB_BLENDCONTROL。
實際target0／07010706解碼為RGB SRC_ALPHA、INV_SRC_ALPHA、ADD，以及alpha
ONE、INV_SRC_ALPHA、ADD，六欄位完整等同固定Plume的AlphaBlend描述。
原始setter本身只更新狀態，不提交GPU命令，也不回傳結果。

`GuestGraphics` 保存四個可分別初始化的 `NativeBlendControl`；未設定的target
不能取得猜測的預設狀態。完整解碼及兩處guest寫入檢查成功後，才執行原始
cache／dirty副作用並保存原生狀態。同值重設仍然標記dirty。

| Target | Guest cache offset | 對device+10的64-bit OR mask |
| --- | --- | --- |
| 0 | 2938 | 20400 |
| 1 | 2958 | 20004 |
| 2 | 295C | 20002 |
| 3 | 2960 | 20001 |

以上皆為十六進位。原有dirty bits完整保留。強符號hook只接受已稽核caller
LR827F5F68，沿用原始void ABI，不改r3為虛構結果碼；其他native-object consumers
仍受保護。實際遊戲本輪只設定target0，其餘target映射由單元測試驗證。

## 支援界限

Decoder支援可直接表達的ADD／SUBTRACT／REV_SUBTRACT及已核對factor，
alpha color-family因子轉為對應alpha因子。只有六欄位都為ONE／ZERO／ADD時，
可將此混色描述視為copy。產生Plume描述時必須另傳入0..15的color write mask。

保留的限制包括：

- 拒絕保留factor／operation編碼與兩組padding bits。
- 拒絕RGB constant-alpha14/15；不可錯用RGB常數取代alpha常數複製。
- 拒絕Xenos MIN/MAX，因其先乘factor，與D3D12忽略factor的MIN/MAX不等價。
- alpha SRC_ALPHA_SAT16尚未納入本版支持範圍，明確拒絕。
- 常數factor保持符號；沒有憑空提供blend constant值。

這個描述只代表per-target blend control。全域alpha-blend enable、color write
mask、blend constants、alpha-to-coverage、render-target格式，以及其他PSO狀態
需由各自原始設定提供。後續真正draw必須組合並使用完整狀態；目前尚未綁定PSO，
不能以setter通過宣稱混色畫面已完成。既有68個shader資源也不等於已完成pixel
shader特殊化或原遊戲繪圖。

## 真實入口證據與驗證

同次 `out/blend-boot.log` 保留原始矩陣、caps及呈現參數核對，以及21次貼圖
建立成功返回；退出碼3的新邊界為：

```text
ORIGINAL_RENDERER_CPU_SETUP device=0x71600000 caps_bytes=304 caps_match=1 parameters_bytes=124 parameters_match=1 width=1280 height=720 vertex_selector=3 pixel_selector=3
NATIVE_BLEND_CONTROL source=0x824e9218 target=0 packed=0x7010706 state=retained-for-pipeline
NATIVE_GRAPHICS_BOUNDARY address=0x824e6a08 lr=0x827f5f88 r3=0x71600000 r4=0x1 r5=0x7010706 r6=0x83e60000 r7=0x83e60000 r8=0x82000000 r9=0x82b60000
STOP native-graphics-function @0x824e6a08: original function cannot consume an unimplemented native object layout
LAST_FUNCTION sub_824E6A08 @0x824e6a08 LR=0x827f5f88 calls=13380
```

呼叫總數受原生工作執行緒排程影響，測試核對實際流程與資料而非固定總數。
來源仍為 `out/recomp/diagnostic-matrix-copy`，原始輸入、ROM及固定vendor沒有修改。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-matrix-copy
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

完整建置及回歸：`out/blend-green-build.log`、`out/blend-ctest.log`（39/39）、
`out/blend-python.log`（156項、無略過）。RED記錄為 `out/blend-entry-red.log`、
`out/blend-red-build.log`、`out/blend-red.log`；兩組targeted GREEN在
`out/blend-green.log`。測試涵蓋所有field位置、factor／operation限制、256種mask、
四target獨立狀態、原始cache與dirty、副作用完整preflight、late guard及live atomic
reservation的拒絕原子性。這些是狀態測試，沒有冒充遊戲畫面。
Decoder及guest/native整合均經獨立唯讀審查。來源commit、執行檔hash及證據索引
保存於 `out/blend-checkpoint.json`。

## 下一個必要依賴

已核對原始呼叫者接下來的七項設定：alpha test enable1、GREATER比較、reference0、
depth enable1、LESS_EQUAL深度比較、depth write1、BACK/CW剔除方向。D3D12沒有
固定功能alpha test，必須由原始pixel shader的特殊化／常數實現；深度enable還要
納入實際attachment。完整原始bytes、hash及狀態欄位記錄在
`out/renderer-states-next-audit.md/.json`。混色原始88-byte函式及校正後語意見
`out/state-824e9218-audit.md/.json`。

依據包含固定Xenia `95a5c3ee250f80c3b9d139658649d9ffb6db3eec` 的
`registers.h`、`xenos.h`及D3D12 pipeline實作，以及固定Plume
`11926860e878e68626ea99ec88562ce2b8badc4f` 的型別與D3D12轉換。
