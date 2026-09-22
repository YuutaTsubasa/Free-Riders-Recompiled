# 補充指令：帶更新的載入/儲存與逐元素向量運算

鎖定版本的 XenonRecomp 不支援兩族指令。它會把這些指令記錄為
unrecognized，並輸出空區塊。之前 `lhzu`、`sthu`、`stfsu` 是逐一改寫的；
這次改成一個表驅動的改寫器 `rewrite_supplemental`，檢查條件和逐一
改寫時相同：

- 該地址的所有日誌事件都必須指明「這個 opcode 不支援」；
- 運算元必須完全符合格式，而且合法（RA≠0；載入時 RA≠RT；位移在16位元
  範圍內；`vspltish` 的立即值在 -16..15）；
- XenonRecomp 為該指令輸出的內容必須是空的（後面可以接標籤）。

不符合的指令會以 `unsupported_supplemental` 停用整個函式，不做部分
替換。上游 XenonRecomp 保持不修改（bootstrap 會驗證）。

## 語意

語意寫在有 C++ 單元測試的輔助函式中，生成碼只插入呼叫。

**[`memory_update_forms`](../src/memory_update_forms.h)**：`lbzux`、`lhzux`、
`lwzux`、`ldux`、`lhau`、`lfsu`、`lfsux`、`lfdu`、`lfdux`、`stbux`、`sthux`、
`stdux`、`stfdu`、`stfdux`。

- EA 以完整64位元計算，記憶體使用低32位元，與既有的 `lhzu` 一致。
- 檢查過的存取成功後才更新 RA，存取失敗時所有暫存器都保持不變。
- `lfs` 系列的單精度轉雙精度完全用整數位元運算完成：非正規數會正規化，
  NaN 的酬載保留、不會被轉成 quiet NaN。因此不受主機的 flush-to-zero
  模式影響，也不會打亂 XenonRecomp 追蹤的 FPU/VMX 模式狀態。

**[`vector_integer`](../src/vector_integer.h)**：`vslh`、`vsrh`、`vsrah`、
`vrlh`、`vsrab`、`vaddsbs`、`vaddsws`、`vadduhs`、`vsubshs`、`vsubuhs`、
`vsububm`、`vmaxsh`、`vminsh`、`vmaxuh`、`vminuh`、`vmaxuw`、`vminuw`、
`vavguh`、`vcmpequh[.]`、`vcmpgtsh[.]`、`vcmpgtuw`、`vcmpgtsw[.]`、`vspltish`、`vsel128`。

- XenonRecomp 會把整個向量反轉存放，所以元素順序是反的，而且每個
  元素內部變成主機端序。逐元素運算只配對同一個主機元素，因此不受
  影響。位移量取自每個元素的低位元。
- 目的暫存器可以和來源重疊：結果會先算在副本中，最後才寫回。
- 帶 record 的比較會設定 cr6：全部為真時 LT=1，全部為假時 EQ=1。
  這和上游的 `setFromMask` 慣例相同。
- 飽和運算和上游一樣，不設定 VSCR[SAT]。

和元素順序有關的指令（`vpk*`、`vslo128`、`vcfpuxws128`）以及 CR 邏輯
運算（`cror`、`crorc`、`bso`、`bns`）尚未處理。

## 結果（相對於[跳轉表](jump-tables.md)之後）

| 項目 | 修改前 | 修改後 |
| --- | --- | --- |
| 停用函式 | 479 | 143 |
| 改寫的指令位置 | 0 | 4,042（無效0） |
| 保留的跳轉表 | 294 | 303 |

剩下的143個函式：55個函式有 RC 比較遺漏事件，其餘受 `bso`、打包類
指令、`sthu`/`lhzu` 的非標準形式等影響。

實跑仍停於 `824EC0A8`，執行過的函式集合與 import 種類和修改前完全相同。
**本次實跑沒有執行到使用新輔助函式的函式**。目前的正確性依據是依
PowerPC 定義手算的 C++ 測試，以及生成器測試。
216項Python（新增9項）與67項CTest（新增2項）通過，無略過。生成器測試
在建置前抓到一個 D 型位移運算元順序的錯誤，已修正。

```powershell
python scripts/generate_diagnostic.py --input out/recomp-switches/ppc --log out/recomp-switches/recompile.log --output out/recomp-switches/diagnostic-vmx --switches out/recomp-switches/switches.toml
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp-switches/diagnostic-vmx
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

`prepare_recomp.py` 從頭產生時也會自動套用這些改寫。
