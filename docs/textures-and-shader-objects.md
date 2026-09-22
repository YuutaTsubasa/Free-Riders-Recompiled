# 紋理綁定、原始著色器物件與第一次 Draw 之前

## SetTexture：原始 fetch constant 合併

`824F4220` 原本只支援經過審核的空紋理綁定。第一張真正的紋理
（LR `824328E8`）的標頭 `+28..+48` 存放 Xenos 紋理 fetch constant。
原始函式會把它合併進裝置的 fetch 槽（`+0x480 + 24*slot`），規則如下：

| 字組 | 來源 |
| --- | --- |
| w0 | 紋理；bits 10..21（夾取模式、sign-RF）保留槽位的值 |
| w1 | 紋理基底位址轉成 GPU 實體位址；bit 11 保留槽位的值 |
| w2 | 紋理（尺寸） |
| w3 | 紋理；bits 19..30（過濾器）保留槽位的值 |
| w4 | 只取紋理的 mip 範圍 bits 2..9，並限制在 [max(紋理下限, `+0x2F5E+slot`), min(紋理上限, `+0x2F78+slot`)] |
| w5 | mip 位址轉換方式同 w1；bits 0..8 保留槽位的值 |

GPU 位址轉換取低29位元；位於 `0xE0000000` 實體視圖時再加 4 KiB。

[`texture_fetch`](../src/texture_fetch.h) 逐位元重現這個合併，並解碼欄位
供日誌與之後的原生紋理使用。被取代的紋理會照原始流程處理：裝置 fence
（`+0x2A9C`）非零時寫入 `old+8`；需要排隊退役（`+0x2AA0` 旗標）時則
明確停止。D3D12 紋理會等到 Draw 實際使用時才建立。

第一張紋理：

```text
NATIVE_TEXTURE_BIND source=0x824f4220 slot=0 ... type=2 format=20 endian=1 dimension=1 size=256x256x1 tiled=0 pitch=8 ... state=fetch-retained
```

也就是 256×256 的 DXT5，線性排列，8-in-16 端序（pitch 8 × 32 = 256）。

## SetVertexDeclaration

`824EBF20` 只把宣告存到 `+11992`（`0x2ED8`），並設定 `+16` 的 bit 19，
因此列入「原始程式碼可直接執行」的白名單。**原生 Draw 必須從這個欄位
讀取目前的宣告。**

## 原始著色器物件

`SetVertexShader`（`824EBD08`）和 `SetPixelShader`（`824EBB00`）會讀取
著色器物件的內部結構：物件 `+872` 的內嵌常數會被複製到裝置的常數影子，
被取代的著色器也會經過和紋理相同的退役流程。原本的原生 handle 是保留但
未提交的頁，無法讀取。

現在建立著色器時，原生 hook 會先驗證容器並取得已編譯的原生著色器，
再呼叫原始函式本體（`__imp__sub_824ED770` / `__imp__sub_824ED588`）
建立真正的客體物件（標頭複製到 `+872`、微碼放在實體記憶體），然後登記
「物件 → 原生著色器」的對應（`NativeShaders::attach`），把物件地址回傳給
遊戲。這兩個設定函式和效果框架的套用函式 `825E8568` 因此都可以直接執行
原始程式碼。**原生 Draw 會從 `+12872`/`+12868` 讀取目前的著色器物件，
再查表取得原生著色器。**

著色器發布檢查改為透過這個對應驗證，仍然回報
`owners_valid=1 stages_valid=1 sources_match=1 unique_handles=1`。

## 其他

- `vcfpuxws128`（`825E85B8`，效果框架）：浮點數 × 2^uimm 截斷成無號字組，
  並做飽和處理；NaN 與非正值得 0。以雙精度計算，所以結果與主機的 flush
  模式無關。停用函式由143降為142。
- 資源銷毀的診斷日誌原本固定讀取52 bytes。小型表面物件可能剛好結束在堆積
  已提交範圍的尾端，所以現在讀不到時會記錄 `inspection=unavailable`，而不是
  由診斷本身中止實跑。
- 原始著色器物件改變了堆積與實體記憶體的配置順序。實跑測試中寫死地址的
  斷言，改為比對結構與地址之間的關係（例如傳輸的表面必須與返回的一致、
  21張紋理依序寫入連續的輸出位置、無鎖串列節點彼此相隔16 bytes）。

## 結果

```text
NATIVE_GRAPHICS_BOUNDARY address=0x824f5288 lr=0x824c08a4 r3=0x71600000 r4=0x6 r5=0x4 r6=0x83e592f8 r7=0x18 ...
STOP native-graphics-function @0x824f5288
```

實跑已到達**第一次 Draw**：`r4=6`（三角形帶）、`r5=4`（頂點數）、
`r6` 為頂點資料、`r7=0x18`（每個頂點24 bytes）。這看起來是 `DrawVerticesUP`
畫一個四邊形。實跑進入的函式共1,136個。

測試：新增 `texture_fetch`（位址轉換、合併、解碼，期望值依原始指令手算），
`guest_texture_binding` 加入有效紋理的合併與退役測試，`vector_integer`
加入 `vcfpuxws`，生成器測試加入 `vcfpuxws128`。69項CTest、216項Python通過，
無略過。
