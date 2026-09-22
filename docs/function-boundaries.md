# 函式邊界：略過填充字組與例外處理常式

XenonRecomp 的線性掃描會把函式之間的非指令字組當成指令解碼，把
`.pdata` 函式切成大量碎片。舊統計中的「14,475 個停用函式」大多是
這類碎片，不是真正的轉譯缺口。

## 原因

- `0x00000000`：函式之間的填充。舊生成中 13,240 筆 `.long` 全是此值。
- 有例外處理的 `.pdata` 函式，前方有兩個字組：處理常式位址與其資料。
  本遊戲只有兩個處理常式，`0x82A5C368`（1557 個函式）與
  `0x82ACB76C`（77 個函式）。其中 `0x82A5C368` 可解碼成 `lwz`，因此
  掃描會吃進函式起點，從此錯位。

兩個處理常式字組在 `.text` 出現的次數，分別與 `.pdata` 例外旗標
的數量完全相同，因此略過它們不會跳過任何可執行指令。`invalid_instructions`
只影響線性掃描，不影響函式本體的轉譯。

## 結果

`config/freeriders.toml` 新增三筆 `invalid_instructions`；
`prepare_recomp.py` 可以輸出陣列與 inline table。

| 項目 | 修改前 | 修改後 |
| --- | --- | --- |
| 函式定義 | 46,503 | 31,082 |
| 停用函式 | 14,475 | 628 |
| `undecoded_instruction` | 13,240 | 2 |
| `error_comment`（跳往無符號位址） | 755 | 152 |

移除的 15,423 個映射起點中，沒有任何 `.pdata` 起點或 `bl` 目標；
所有 `.pdata` 起點與 `.text` 內 `bl` 目標在新生成中都存在。

實跑仍停於 `824EC0A8`（LR `8249BC68`），ENTER／IMPORT 事件種類與
修改前相同；日誌行數差異只在等待與臨界區次數（執行緒時序）。
65項CTest、195項Python測試通過，無略過（需設定
`SFR_SOURCE_XEX`、`SFR_IMAGE_DUMP`、`SFR_CPU_DIAGNOSTIC`、
`SFR_IMAGE_DIRECTORY`、`SFR_ASSET_DIRECTORY`）。

## 剩餘：switch 表

剩下的152個 `ERROR` 全在 `.pdata` 範圍外，主因是 switch 跳轉表未被
辨識：表格直接放在 `.text`，被當成指令解碼。XenonAnalyse 的絕對
跳轉表樣式為 `lis, addi, rlwinm, lwzx, mtctr, bctr`，本遊戲編譯器產生
`lis, rlwinm, addi, lwzx, mtctr, bctr`（177處），另有約110處含 `ori`
的偏移表變體，因此 `switches.toml` 為0筆。已由 [跳轉表](jump-tables.md) 處理。

```powershell
python scripts/prepare_recomp.py --output out/recomp-boundaries
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp-boundaries/diagnostic
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```
