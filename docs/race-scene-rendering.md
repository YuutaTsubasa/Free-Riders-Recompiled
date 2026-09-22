# 比賽畫面：算繪目標的別名與解析出來的貼圖

日期：2026-09-20。分支 `claude/function-boundaries`。接續
[骨骼調色盤與迴圈常數](shader-vertex-palette.md)。

著色器都翻譯得出來之後，比賽場景確實畫進了原生的框架緩衝（解析時傾印可以看到完整畫面），
但螢幕上只剩 2D。逐繪製傾印一格比賽畫面（`SFR_FRAME_DUMP`）顯示畫面在**第 246 次繪製**
變全黑：一個 4 頂點、不混色、不做深度測試的全螢幕四邊形，也就是最後的合成。它取樣的
貼圖是 `0x0AA4E000` 等位址，而遊戲解析（resolve）的目的地記錄成 `0xEAA4D000`。

兩者其實是同一塊記憶體：實體配置在 `0xE0000000` 視圖裡的位址是**實體位址 + 0xE0000000
− 0x1000**。解析時我們用表面物件裡的抓取常數（虛擬位址）當鍵，繪製取樣時用的是實體
位址，所以查不到那份複本，合成只好畫出全黑，把場景蓋掉。把解析目的地換算成實體位址
之後（[guest_graphics_hooks.cpp](../src/guest_graphics_hooks.cpp) 的 `824FAB08`），
`NATIVE_RESOLVED_SAMPLED` 開始出現，比賽畫面就完整顯示了。

## 一格比賽畫面的結構

| 繪製 | 目標 | 內容 |
| --- | --- | --- |
| 1..45 | 遊戲自己的表面 | 陰影貼圖（深度，768×768） |
| 46..245 | 原生色彩表面 | 天空清除（0x0A4BA9）、場景、角色 |
| 246.. | 遊戲自己的表面 | 後製（1/2、1/4 縮圖與模糊，110×90、55×45 等） |
| 最後 | 原生色彩表面 | 合成的全螢幕四邊形，然後 HUD |

原生後端只有一組框架緩衝，所有算繪目標都別名到它（`SFR_ALLOW_RENDER_TARGETS=1`）。
每個 pass 畫完之後遊戲都會解析成貼圖，下一個 pass 再取樣那份貼圖，所以只要解析的複本
對得上位址，整條鏈就能在單一框架緩衝上跑完。`SFR_SKIP_FOREIGN_TARGETS=1` 可以改成
丟掉所有畫向遊戲自己表面的繪製與清除，只留下場景 pass（追查用）。

## 880×720 的後緩衝：呈現時拉伸

進入比賽前遊戲會把裝置重設成 **880×720** 的後緩衝（`NATIVE_DEVICE_RESET_PARAMETER
word=0 value=0x370`，多重取樣型別 1），真機由顯示縮放器拉伸到 1280×720。原生的框架
緩衝是 `CreateDevice` 當時的 1280×720，本來直接一比一複製到交換鏈，所以以後緩衝為
座標的畫面（例如「Now Loading」，視區 880×720）只佔了視窗左邊 880/1280 ≈ 69%。

但**不能**把框架緩衝縮成後緩衝大小：記錄顯示比賽的 68009 筆視區全是 1280×720（後
緩衝才是 880×720），縮小會把場景裁掉 31%。在視區上縮放也不行，試過兩種都失敗：

- 「視區剛好等於後緩衝時就放大到整個框架緩衝」：只有部分 pass 符合條件，螢幕空間的
  pass（`g_ScreenSpaceScale`）又是另一套座標，畫面變成左右兩半各自為政。
- 「依目前綁定表面的尺寸縮放」：算繪目標表面的 +28 沒有可用的尺寸（讀出來是
  `11797193x1` 之類的亂數），而且載入時會出現 `0xFFFF×0xFFFF` 的視區，先夾到框架
  緩衝再放大就超出 D3D12 範圍，執行直接停住。

現在改在**呈現時**做，跟真機一樣：`NativePresentation::present(area)` 在指定範圍小於
框架緩衝時，用一個全螢幕三角形把該範圍取樣拉伸到交換鏈貼圖（著色器由
[native_shader_compiler.cpp](../src/native_shader_compiler.cpp) 以 Windows SDK 的 DXC
即時編譯，取樣範圍走 push constant），否則維持原本的一比一複製。範圍由
`GuestGraphics::present_front_buffer` 決定：**這一格最後一個 pass 的視區剛好等於
後緩衝**時就用後緩衝的大小，否則用整個框架緩衝。`SFR_PRESENT_STRETCH=0` 可關閉。

結果：載入畫面填滿視窗，標題、選單與比賽（視區 1280×720）維持原樣。螢幕截圖
（`SFR_SCREENSHOT`）也套用同樣的拉伸，所以截圖與視窗看到的一致。

## 仍然是近似的地方

- 解析出來的複本一律是**整個框架緩衝**的大小，不是解析區域，也不會縮小；縮圖鏈因此
  不是真的縮圖。
- 深度解析（陰影貼圖）沒有複製出來。
- 沒有格式對應的貼圖用 1×1 白色代替。
- 真正的離屏算繪目標仍然沒有，上面這些都要等它。

## 驗證

```bash
SFR_SKIP_MOVIES=1 SFR_INPUT_AFTER_PRESENT=1800 SFR_INPUT_PRESENT_CLOCK=1 \
SFR_INPUT_SCRIPT="start@3+0.2,a@21+0.2" SFR_ALLOW_RENDER_TARGETS=1 \
SFR_SCREENSHOT=out/race-%d.bmp SFR_SCREENSHOT_EVERY=240 SFR_PRESENT_LIMIT=8000 \
out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

教學關的比賽畫面（第 5760 格）有完整的 3D 場景與 HUD：Sonic、板子、賽道、終點門、
天空與草地，環數、等級、計時器、路線圖與道具槽。

## 追查用的開關

- `SFR_FRAME_DUMP=<present>`／`SFR_FRAME_DUMP_EVERY=N`：那一格畫面每 N 次繪製寫一張
  BMP，並記下著色器、目標、頂點數與取樣的貼圖位址。
- `SFR_RESOLVE_DUMP=<檔名%d.bmp>`／`SFR_RESOLVE_DUMP_AFTER=<present>`：解析當下的
  框架緩衝。
- `SFR_TEXTURE_SURVEY=N`：前 N 個被取樣的貼圖位址。
- `SFR_SKIP_FOREIGN_TARGETS=1`：丟掉畫向遊戲自己表面的繪製與清除。
