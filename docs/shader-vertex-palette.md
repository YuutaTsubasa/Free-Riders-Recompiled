# 骨骼調色盤與迴圈常數：讓 3D 著色器翻譯得出來

日期：2026-09-20。分支 `claude/function-boundaries`。

比賽場景進得去之後，畫面上只有 2D（HUD、路線圖、計時器），3D 完全不見。原因不是
算繪目標，而是**大部分頂點著色器根本翻譯不出來**：1182 個執行期著色器裡有 735 個被
釘選的 XenosRecomp 拒絕。修好之後 3D 場景確實畫出來了（見最後一節）。

## 一、宣告之外的頂點抓取（骨骼調色盤）

735 個失敗裡有 726 個是同一個斷言：

```
Assertion failed: findResult != vertexElements.end(), shader_recompiler.cpp, line 195
```

翻譯器只認得**頂點宣告列出的** vfetch 指令位址；Sonic Free Riders 的蒙皮著色器除了
宣告過的屬性之外，還自己寫了一組 vfetch，指令裡就帶著格式與間距：

| | 宣告過的抓取 | 多出來的抓取 |
| --- | --- | --- |
| fetch 常數 | 95（串流 0） | 94（串流 1） |
| 格式 | 0（由驅動依宣告填入） | 38（`FMT_32_32_32_32_FLOAT`） |
| 間距 | 0（同上） | 16 dword＝64 位元組 |
| 位移 | 0 | 0／4／8／12 dword |

也就是每筆 64 位元組（4 個 float4）的**矩陣**，索引放在某個暫存器的某個分量裡：
標準的矩陣調色盤蒙皮。翻譯器不支援這種用法。

上游的 `XenosRecomp` 必須與釘選版本逐位元組相同（`scripts/build_shader_translator.ps1`
會檢查），所以修正全部做在我們自己的前處理與後處理（[vertex_palette.cpp](../src/vertex_palette.cpp)）：

1. 依翻譯器自己的走法掃描控制流（前置掃描先把控制流程式的長度縮到第一個被執行的
   指令位址，再逐個 exec 取出每筆指令的兩個 sequence 位元），找出宣告之外的 vfetch。
   mini fetch 只帶目的暫存器與位移，常數、格式、間距與索引來源沿用前一個完整 fetch。
2. 把這些位址當成**一個沒人用的輸入**加進宣告（優先選 `POSITION3`／`POSITION2`／
   `POSITION1`／`COLOR0` 裡該著色器沒用到的一個；必須是 XenosRecomp 的 `USAGE_LOCATIONS`
   有的組合，否則它的 Metal 路徑會直接 `exit(1)`）。改寫過的著色器結構整個接在容器
   尾端，只把容器的 shader 位移指到新的一份，微碼原地不動。
3. 翻譯之後，把產生出來的 `input.i<那個輸入>` 依序換成
   `sfrVertexPalette(int(r<n>.<c>) * 4 + <列>)`，並刪掉該輸入的宣告行。改寫的筆數
   與 fetch 數不符就放棄（著色器維持不可翻譯），不會默默接受。

調色盤本身由繪製時上傳：串流 1 的頂點抓取常數在 device + 0x770（串流 n 在
device + (239−n)×8，是原始 `SetStreamSource`（824E8DB8）算出來的位置），內容整筆
換位元組後放進 `b3, space4` 的常數緩衝區（1024 個 float4）。

## 二、繪製時才設定的迴圈常數

剩下的失敗（168 個）是 `use of undeclared identifier 'i0'`：著色器用
`for (aL = 0; aL < i0.x; aL++)` 迴圈跑骨骼，但翻譯器只宣告**容器裡內嵌**的整數常數，
這些的數值是遊戲用 `SetVertexShaderConstantI` 在繪製時才設定的。

[loop_constants.cpp](../src/loop_constants.cpp) 在翻譯後的 HLSL 裡找出「用到但沒宣告」的
`i<N>`，補上 `int4 i<N> = g_LoopConstants[N];`，並宣告 `b4, space4` 的常數緩衝區。
繪製時從 device + 10140 讀 16 個暫存器（每個一個 dword，count／start／step 各一個
有號位元組）展開成 int4 上傳。

## 三、TEXCOORD8 與重複的輸入宣告

最後 3 個失敗是 `Missing mapping for vertex element usage: TexCoord 8`：翻譯器只認得
`USAGE_LOCATIONS` 裡的 (用途, 索引)，而 `shader_input_usage` 原本只把 TEXCOORD4-7 改名。
這些著色器還把 TEXCOORD6/7/8 各宣告了 4 次（同一個屬性被 4 個 fetch 指令引用，容器是
每個 fetch 一筆），改名之後輸入結構會重複宣告同一個成員。

改成 TEXCOORD4-12 都改名（POSITION1-3、NORMAL1-3、TANGENT1-3），並在翻譯後把輸入
結構裡重複的成員去掉（[shader_inputs.cpp](../src/shader_inputs.cpp)，兩個前置處理分支
各自去重）。

## 結果

- 1182 個執行期著色器原本有 735 個不可翻譯，**現在 0 個**；比賽場景的
  `NATIVE_DRAW_SKIPPED` 從 57 降到 0。
- 比賽中在**解析（resolve）時**把框架緩衝區傾印出來，可以看到完整的 3D 場景：
  Sonic、板子、賽道、建築、天空、草地，蒙皮正常。

```bash
SFR_SKIP_MOVIES=1 SFR_INPUT_AFTER_PRESENT=1800 SFR_INPUT_PRESENT_CLOCK=1 \
SFR_INPUT_SCRIPT="start@3+0.2,a@21+0.2" SFR_ALLOW_RENDER_TARGETS=1 \
SFR_RESOLVE_DUMP=out/resolve-%d.bmp SFR_RESOLVE_DUMP_AFTER=5200 SFR_PRESENT_LIMIT=7000 \
out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

## 接下來

當時畫面上仍然只有 2D：合成階段沒有取樣回解析出來的貼圖。原因是解析目的地與繪製取樣
用了不同的位址空間，修好之後比賽畫面就完整顯示了，見
[比賽畫面：算繪目標的別名與解析出來的貼圖](race-scene-rendering.md)。
