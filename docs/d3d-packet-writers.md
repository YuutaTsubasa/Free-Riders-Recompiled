# D3D 盤點與「只寫封包」函式

## 為什麼要改做法

遊戲靜態連結了 Xbox D3D 函式庫（約 `0x824E4000–0x8250D000`）。原生裝置
（`0x71600000`）取代了原始裝置，所以任何以它為 r3 的原始函式都會被
`native-graphics-function` 攔下，除非它已有原生實作。

[`scripts/inventory_d3d.py`](../scripts/inventory_d3d.py) 盤點遊戲從函式庫
外部直接呼叫的 D3D 函式：

| 項目 | 數量 |
| --- | --- |
| 遊戲直接呼叫的 D3D 函式 | 208（2,049 個呼叫點） |
| 已有原生處理 | 36 |
| 本次實跑進入過 | 48 |
| 進入過但沒有原生處理（操作資源物件，不是裝置） | 22 |

照「撞到一個停止點、補一個函式」的節奏，離標題畫面還很遠。

## 可丟棄的命令緩衝區

很多 D3D `Set*` 函式只做三件事：

1. 更新裝置的影子欄位；
2. 經由 `device+48`（先遞增的寫入指標）寫入 Xenos PM4 封包；
3. 設定髒旗標。

寫入指標超過 `device+56` 時，會呼叫 `0x824F8720` 把命令段送交 GPU。

現在原生裝置會映射一塊 64 KB 的暫存區（`0x71710000`），`+48` 指向開頭、
`+56` 保留 16 KB 餘裕。原生的 `0x824F8720` 只丟棄已寫入的封包，並回傳
重設後的指標（`NATIVE_COMMAND_DISCARD`）。一次寫入超過餘裕會落到未映射
的記憶體，程式會明確停止。

只有經過審核的函式可以在原生裝置上執行原始程式碼（`original_packet_writer`）：

| 地址 | 內容 | 審核結論 |
| --- | --- | --- |
| `824EC0A8` | `SetShaderGPRAllocation(flags, vs, ps)`：更新 `+10920`，寫封包 | 在 D3D12 上沒有對應概念（MarathonRecomp 也直接空實作） |
| `824EB5D0` | 在 `+16/+24/+32` 標記與著色器相關的髒狀態，尾呼叫 `824E66B8` | 只修改旗標 |
| `824E66B8` | 由 `+10436/+10440` 產生 screen scissor 封包 | 除了寫入指標之外，不修改任何欄位 |

它們影子更新的狀態，要由原生實作讀取後才會影響畫面。如果之後的原生
Draw 需要這些狀態，會從裝置欄位讀取。

另外，`__save*`/`__rest*` 暫存器輔助函式（名稱只指派給設定檔中已驗證
的地址）改為依名稱整類排除，不再逐一列地址。

## 覆蓋率追蹤

設定 `SFR_FUNCTION_TRACE=<檔案>` 後，診斷程式結束時會寫出所有進入過的
函式地址。`ENTER` 只記錄前100次呼叫，不能代表覆蓋率。本次實跑共進入
1,066個函式。

## 結果

實跑越過 `824EC0A8`（`SetShaderGPRAllocation(0,0,0)` 完整執行，封包未超過
暫存區，所以沒有觸發丟棄），現在停在同一個呼叫者的下一個呼叫
`824E65A0`：

```text
STOP native-graphics-function @0x824e65a0: original function cannot consume an unimplemented native object layout
```

`824E65A0` 會保存視口和剪裁區域，接著呼叫疑似 `SetRenderTarget`
（`824E97F8`）、`SetDepthStencilSurface`（`824E5AE8`）、Clear/Resolve
（`824FAB08`），最後再還原。這需要原生的渲染目標語意，不能列入白名單。

測試：67項CTest（`guest_graphics` 新增暫存區與丟棄的測試）、216項
Python（實跑測試新增不再停在上述地址的斷言），全部通過，無略過。

```powershell
$env:SFR_FUNCTION_TRACE = 'out/d3d-inventory/function-trace.txt'
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
cd scripts; python inventory_d3d.py ../out/recomp/image/image.bin --function-trace ../out/d3d-inventory/function-trace.txt --output ../out/d3d-inventory/inventory.json
```
