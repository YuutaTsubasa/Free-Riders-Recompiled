# Switch 跳轉表

XenonAnalyse 只比對固定的指令順序。它預期的絕對跳轉表是
`lis, addi, rlwinm, lwzx, mtctr, bctr`，本遊戲編譯器產生的卻是
`lis, rlwinm, addi, lwzx, mtctr, bctr`，因此 `switches.toml` 一直是0筆。
表格直接放在 `.text` 時，還會被當成 `lwz` 指令翻譯。

## 分析方式

[`scripts/analyse_switches.py`](../scripts/analyse_switches.py) 取代
XenonAnalyse，不寫死指令順序：

1. 對每個 `bctr`，往回找 `bgt crN`（或 `bgtlr crN`）及支配它的
   `cmplwi crN,rX,N`。
2. 往回推導在比較點上「等於索引加常數」的暫存器（經 `mr`／`addi`）。
3. 對索引 0..N 實際模擬比較點到 `bctr` 的直線程式碼，從解碼映像讀取
   表格，得到每個 case 的目標。
4. 以下任一情況都拒絕，不猜測：無法建模的指令、目標依賴未知暫存器、
   表格不在唯讀映像內、標籤不在 `bctr` 所屬的 `.pdata` 函式內。

四種形式都用同一套邏輯處理：絕對地址表、半字偏移表、位元組偏移×4、
位元組偏移。

**交叉驗證：** 對先前人工核對的 `COUNTRY_CTR`（38個目標）與
`FORMAT_CTR`（15個目標），分析結果完全相同。這兩處仍保留原本以
雜湊鎖定的處理，不寫入 `switches.toml`。

## 分派方式

XenonRecomp 會生成 `switch (ctx.rN.u64)`，default 為
`__builtin_unreachable()`。診斷生成器會逐字核對這段輸出，再改成依
原始程式剛算出的 CTR 目標分派：

```cpp
switch (ctx.ctr.u32) {
case 0x822351F0: goto loc_822351F0;
...
default: throw sfr::RuntimeStop("jump-table-target", ctx.ctr.u32, "...");
}
```

這完全對應原始語意（跳到算出的地址），不必信任索引暫存器或它的
高32位元。其他路徑匯入派發序列時，索引可能沒有經過邊界檢查，
但任何不在已驗證標籤中的目標都會明確停止，不會跳到未驗證的位置。
輸出只要和預期不符，該函式就以 `unsupported_jump_table` 停用，
不做部分替換。

## 沒有 `.pdata` 的函式

43個表位在沒有 `.pdata` 的葉函式裡。XenonRecomp 自行推測這些函式的
範圍時，會在 `bctr` 截斷，所有 case 標籤都落在函式外。
`table_functions` 會從入口（優先選 `bl` 目標，其次是程式碼邊界）做
可達性分析：跟隨分支與跳轉表標籤，範圍不超過下一個 `.pdata` 起點。
37個算出的範圍由 `prepare_recomp.py` 以 `functions` 傳給 XenonRecomp。

## 結果（相對於[函式邊界](function-boundaries.md)修正後）

| 項目 | 修改前 | 修改後 |
| --- | --- | --- |
| 辨識的跳轉表 | 0 | 309（另有2個保留人工處理） |
| 保留並轉換的跳轉表 | 0 | 294 |
| 停用函式 | 628 | 479 |
| `error_comment` | 152 | 2 |

未保留的15個表中，14個轉換正確，但所在函式因其他不支援的指令被停用。
剩下1個是 `82980CDC`：它所在的函式 `82980C78` 前面是填充字組，
內部入口 `82980CA8` 是 `bl` 目標，分析選了後者，所以外層函式被拒絕。
如果真的呼叫到外層，會明確停止。

實跑仍停於 `824EC0A8`，執行過的函式集合與 import 種類和修改前完全相同。
**本次實跑沒有走到任何新的跳轉表**。目前的正確性依據是：和兩處人工
核對結果一致，加上單元測試。
207項Python（新增12項）與65項CTest通過，無略過。

```powershell
python scripts/prepare_recomp.py --output out/recomp-switches
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp-switches/diagnostic
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

證據：`out/recomp-switches/switches.json` 列出每個被接受的表（邊界、
匯入點、所屬函式）、每個被拒絕的 `bctr` 與原因，以及算出的函式範圍。
