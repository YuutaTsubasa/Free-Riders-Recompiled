# 原始混色預設值與狀態更新

Windows 真實啟動已通過原始混色啟用、來源與目的因子設定，接著停於
`824E8248`，LR `82211750`，參數為 device、0、1。**尚無原始 Draw、
Present、標題畫面或確認後進入主選單的證據。**

先前原生裝置只安裝 render-state dispatch table，沒有套用其中混色
預設值。原始 `824F4B28` 將裝置配置清零，`82500B38` 隨後按順序呼叫
`82AD0A60` 的101個預設 setter。核對其中所有11個相關 setter 後，
requested packed word（device+2EF8）為 `00010001`，shared flags
（+2EFC）為 `00240000`，四個 effective target words 為 `00010001`。
flags 的低位元亦包含三個較後方的預設值，不能全部設零。

原生建立前驗證這11筆原始 setter／value，再從快照推導 requested
words，建立四份 copy blend controls。這些值取自原始資料，不從較後
的 target0 `07010706` 倒推。其他尚未實作的 typed states 仍未設定。
這是現有原生裝置契約的預設值提取，並未執行整段原始 CreateDevice；
其任意 certification/debug callbacks 不在支援範圍內。Debug monitor
已有明確缺席後端，cert monitor 匯入仍未支援，沒有把原始 IAT token
當成零值或假裝執行 callback。

新增三個完整 ABI setter：

| 原始入口 | 狀態 | 原始效果 |
|---|---|---|
| 824E6A40 | ALPHABLENDENABLE | 改 flags bit31，重建四個 effective controls |
| 824E6B60 | SRCBLEND | 改 requested bits0..4；enabled 時重建 controls |
| 824E6BF0 | DESTBLEND | 改 requested bits8..12；enabled 時重建 controls |

未啟用 separate-alpha 時，依原始 rotate/mask 重建 alpha half；啟用時
直接使用完整 requested word。停用混色產生 ONE/ZERO/ADD copy，保留
requested factors。保留原始 DWORD 行為：enable=2 清除 bit31，但
因參數不等於零仍走 effective 更新分支。factor 只使用低5位元。

啟用 setter 按原始順序寫入 target2、0、1、3，factor setters 為
target0、1、2、3；dirty qword 依序 OR
400、4、2、1，總計 `407`；不加入另一個 raw-target setter 的 `20000`。
重複相同值仍標記 dirty。停用時改 factor 只寫 requested word，完全
不存取 effective targets 或 dirty word。先檢查所有實際寫入並 decode，
再發布 guest／native 狀態；不支援的有效因子或操作在寫入前停止。
觀察日誌使用已取得的結果，不增加 guest memory 讀取。

同一次實跑可見：

```text
NATIVE_BLEND_DEFAULTS table=0x82ad0a60 requested=0x10001 flags=0x240000 effective=0x10001 targets=4
NATIVE_BLEND_REQUEST source=0x824e6a40 value=0x1 requested=0x10001 flags=0x80240000 effective=0x10001 lr=0x8221171c state=retained-for-pipeline
NATIVE_BLEND_REQUEST source=0x824e6b60 value=0x6 requested=0x10006 flags=0x80240000 effective=0x60006 lr=0x82211734 state=retained-for-pipeline
NATIVE_BLEND_REQUEST source=0x824e6bf0 value=0x7 requested=0x10706 flags=0x80240000 effective=0x7060706 lr=0x82211740 state=retained-for-pipeline
STOP native-graphics-function @0x824e8248: original function cannot consume an unimplemented native object layout
```

原始 caller 保留執行，alpha-test disable 也在原本兩個混色呼叫間執行。
這些 setter 沒有 GPU submission；未來真正 draw 的 PSO 必須使用全部
effective controls、write masks、formats 及必要 blend constants。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

建置／測試／實跑日誌：`out/blend-requests-{build,ctest,python,boot}.log`；
來源／執行檔／證據雜湊：`out/blend-requests-checkpoint.json`。
原始指令與資料核對：`out/graphics-824e6a40-next-audit.md`、
`out/blend-cache-initialization-audit.md` 及其 JSON。

64項CTest及192項Python測試全部通過，無略過。新增案例包含原始預設值
驗證失敗、四目標完整狀態、enable=2、factor masking、separate-alpha、
重複 dirty、readonly provider、未生效欄位不存取、reservation及失敗
不部分更新。獨立審查修正日誌多讀取及factor setter寫入順序；修正後
完整測試與真實啟動均通過。最新啟動仍以exit3在未支援函式停止。

下一依賴核對在 `out/graphics-824e8248-next-audit.md`：min/mag/mip
取樣設定包含原始 inline 寫入，未來 native sampler 必須消費其最終
真實 cache；不能僅靠兩個 setter hooks 留下過期的 typed state。

硬體欄位參考：[固定 Xenia RB_BLENDCONTROL](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/registers.h#L780)。
原始資料為本機已驗證映像，不隨來源提交。
