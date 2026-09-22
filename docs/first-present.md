# 原遊戲的第一次 Present

## 辨識

先前的停止點 `824E65A0` 由遊戲的虛擬方法 `8249BC40` 呼叫，夾在兩次
`SetShaderGPRAllocation` 之間。另一個呼叫者是 `82828A80`。它的內容如下：

1. 把視口（`+12824`，28 bytes）與剪裁區域（`+12852..+12864`）存到堆疊；
2. `824E97F8(dev, 0, +15044)`：這是 `SetRenderTarget`，把表面存到
   `+12616+4*index`，並從表面 `+28` 讀取格式來更新影子欄位；
3. `824E5AE8`：寫入等待/同步用的封包；
4. `824FAB08(dev, 0, NULL, +15040, …)`：Resolve，把 RT0 複製到前緩衝區
   （原始 `CreateDevice` 由 `824F3EA0` 建立的紋理），flags 為0，沒有清除；
5. 還原視口（`824E9748`）、剪裁區域（`824E8C70`）；
6. `824E5F18(dev, +15040, +13852)`：交換前緩衝區。這條路徑會呼叫
   `VdSwap`、`VdPersistDisplay`、`VdGetSystemCommandBuffer`，另外也由 D3D
   內部的 swap 工作執行緒 `8250B228` 使用。

原始 `CreateDevice`（`82500DA0`）的設定可以對照：`+15044` 是
`CreateRenderTarget`（`824F3FC0`）建立的後緩衝區，`+15036` 是深度，
`+15040` 是前緩衝區；接著呼叫 `SetRenderTarget(dev, 0, bb)` 與
`SetDepthStencilSurface`（`824E9B48`）。原生 `CreateDevice` 則把這三個欄位
設成原生 handle。

## 原生實作

`PPC_FUNC(sub_824E65A0)` → `GuestGraphics::present_front_buffer`：只接受
後緩衝區 `+0x3AC4` 與 RT0 `+0x3148` 都是原生色彩 handle、前緩衝區
`+0x3AC0` 是原生呈現 handle 的情況，然後把原生色彩目標呈現到交換鏈。
其他組合都會明確停止。

原始函式結束時，視口、剪裁區域與 RT0 都和呼叫前相同，所以原生版本
不修改這些欄位。**限制：**原始 swap 路徑還會更新 swap 計數
（`+16776`）、顯示旗標（`+10944`）等欄位，原生版本目前沒有更新。之後
如果有原始程式碼讀取這些欄位，會需要補上。

## 結果

```text
NATIVE_PRESENT source=0x824e65a0 device=0x71600000 lr=0x8249bc70 presented=1
IMPORT_CONTEXT name=__imp__XamInputGetState address=0x82acc14c lr=0x824c64d0 ...
STOP worker-import-function @0x82acc14c: __imp__XamInputGetState [guest_id=16 function=sub_82A70D30 ...]
```

第二次 `SetShaderGPRAllocation(0,66,62)` 與後續的虛擬呼叫都執行完畢，
遊戲接著在第16號工作執行緒上輪詢手把輸入。實跑進入的函式由1,066個增加
到1,111個。

這是原遊戲的第一次 Present。到目前為止畫面上只有原生清除的結果，還沒有
任何原遊戲的 Draw。

測試：`guest_graphics` 新增 Present 的接受與拒絕測試；實跑測試新增
`NATIVE_PRESENT` 斷言。67項CTest、216項Python通過，無略過。
