# 原生輸入與 BLENDOP

## XamInputGetState

第一次 Present 之後，遊戲在第16號工作執行緒上輪詢
`XamInputGetState(user, flags, XINPUT_STATE*)`（LR `824C64D0`）。

[`native_input`](../src/native_input.h) 的實作如下：

- **手把**：Windows XInput。Xbox 360 XAM 和 XInput 的按鍵位元值相同，
  結果以大端序的 `XINPUT_STATE` 寫回（封包序號、按鍵、兩個扳機、四個
  搖桿軸，共16 bytes）。Linux 改用 SDL GameController（依連線順序為玩家0–3，
  PlayStation 手把也經由它），扳機與 Y 軸換算成 XInput 的範圍與方向，震動用
  `SDL_GameControllerRumble`（見 [Linux 版](linux.md)）。
- **鍵盤**：只有在遊戲視窗位於前景（Linux 為 SDL 鍵盤焦點）時才讀取，並和玩家0的手把狀態合併
  （按鍵取聯集，搖桿軸取離中心較遠的值）。所以玩家0永遠回報為已連線。

  | 鍵盤 | 對應 |
  | --- | --- |
  | 方向鍵 | 十字鍵與左搖桿 |
  | Enter | START |
  | Tab | BACK |
  | Z／空白鍵 | A |
  | X／Backspace／Esc | B |
  | C | X |
  | V | Y |
  | Q／E | LB／RB |

- 封包序號只在狀態改變時遞增，符合 XInput 的語意。
- flags 只接受 0 或 `XINPUT_FLAG_GAMEPAD`（1），而且輸出位址不可為空，
  玩家編號須為0..3。其他情況都會明確停止。
- 輪詢每一幀都會發生，所以日誌只記錄第一次查詢和每一次狀態改變
  （`NATIVE_INPUT`）。

## SetRenderState(D3DRS_BLENDOP)

`824E6AD0` 和已有原生實作的 `DestBlend`（`824E6BF0`）逐條相同，唯一的
差別是插入位元的指令：`rlwimi r10,r4,5,24,26`，也就是
`(value & 7) << 5`。因此把它加入 `BlendRequest::operation`，並列入
已審核的渲染狀態入口。

## 結果

```text
NATIVE_INPUT user=0 status=0x0 packet=0 buttons=0x0 lr=0x824c64d0 backend=xinput+keyboard
NATIVE_BLEND_REQUEST source=0x824e6ad0 value=0x0 requested=0x10706 flags=0x80240000 effective=0x7060706 lr=0x8249b83c ...
NATIVE_GRAPHICS_BOUNDARY address=0x824f4220 lr=0x824328e8 r3=0x71600000 r4=0x0 r5=0x4006ff00 ...
STOP native-graphics-function @0x824f4220
```

現在停在 `SetTexture`。這是第一次綁定真正的紋理（`0x4006FF00`，由原始
程式碼建立）；先前只支援經過審核的空紋理綁定（LR `827F5FC8`）。實跑測試
原本斷言「不會停在 `824F4220`」，現在改成「空紋理綁定不會在此停止」。

測試：新增 `native_input`（鍵盤映射、合併、大端序配置、封包序號、玩家
連線狀態），並在 `guest_blend_request` 加入 operation 的測試；實跑測試
加入輸入和 BLENDOP 的斷言。68項CTest、216項Python通過，無略過。
