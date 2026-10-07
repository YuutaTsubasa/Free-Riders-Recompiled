# 操作設定（啟動器的 Controls 分頁）

誰用什麼玩、哪個按鍵做什麼，都在啟動器的「操作」分頁裡設定，存在
`settings.ini`，開始遊戲時以環境變數傳給遊戲。

此分頁提供於 Windows／Linux 桌面啟動器。Android 的 Java 啟動介面維持原樣，
沒有新增操作設定頁；Android 原生執行階段只在收到對應環境變數時套用這些設定。

## 誰用什麼

| 設定 | 選項 | 意思 |
| --- | --- | --- |
| 1P | 手把與鍵盤（預設）／手把／鍵盤 | 第一位玩家。鍵盤一直都在手把後面備著 |
| 2P | 手把（預設）／鍵盤／沒有 | 第二位玩家 |

**本次發行保留 1P／2P 的輸入來源、按鍵綁定與手把選擇設定，但不新增可遊玩的雙人模式或分割畫面。**
2P 設定只配置第二組輸入；選擇鍵盤或插入第二支手把不會啟用雙人遊戲。

## 哪一支手把是誰的

1P 與 2P 各有一個「用哪一支手把」的選項，列出**目前插著的手把**，可以直接指名：

- **自動（依順序）**：預設。誰先被電腦列出來誰就是 1P。
- **指名某一支**：只要它插著就是那位玩家的，不管電腦把它排在第幾個；**拔掉的話
  那位玩家就等它**，不會去搶別人手上那支。

手把的名字在 Windows 上是 XInput 的「Controller 1..4」，以及走 HID 讀的
「PlayStation controller」；Linux／Android 用 SDL 給的名字，同名手把會加上編號。
SDL 的選擇設定會保存裝置 GUID 與序號識別，讓啟動器與遊戲能找到同一支手把；
沒有序號的手把則依同型號裝置的列舉順序編號。**沒有序號的同型號手把若以不同順序重新連接，
可能需要重新選擇。** 舊設定中的 SDL 名字仍可使用，但必須只有一支符合，避免選錯手把。
Windows 版遊戲啟動時會把實際
分配印在記錄裡，想確認名字看這行就好：

```text
NATIVE_PAD player=1 name="PlayStation controller" chosen=asked
NATIVE_PAD player=2 name="Controller 1" chosen=order
```

`chosen=asked` 是照你指名的，`chosen=order` 是依順序拿到的。

## 預設按鍵

兩位玩家共用一個鍵盤，所以兩組預設鍵不會互相撞到：

| 功能 | 1P | 2P |
| --- | --- | --- |
| 十字鍵／左搖桿 | 方向鍵 | W A S D |
| A ／ B | Z ／ X | G ／ H |
| X ／ Y | C ／ V | T ／ Y |
| LB ／ RB | Q ／ E | 1 ／ 2 |
| LT ／ RT | R ／ F | 3 ／ 4 |
| Start ／ Back | Enter ／ Tab | 5 ／ 6 |
| 右搖桿（Kinect 游標） | I J K L | 數字鍵盤 8 4 2 6 |

手把預設就是手把自己：A 是 A、B 是 B。

## 換按鍵

在「設定按鍵」選一組（1P 鍵盤／1P 手把／2P 鍵盤／2P 手把），點某個功能的按鈕，
再按下想用的鍵或按鈕就換掉了。等待按鍵的時候，選單的方向鍵導覽會暫時關掉——
不然同一次按下會一邊綁定一邊把焦點移走。
手把綁定會讀取所有已連接手把的新按下事件，因此也可以直接使用第二支手把設定；
用來開啟綁定的按鈕須先放開，再按下才會成為新綁定。

兩件要知道的事：

- **一個按鈕被指派給別的功能之後，就不再做自己原本的事。** 把 Start 綁到 LB，
  LB 就是 Start，不會變成「兩個都按到」。
- **手把的類比搖桿不能換。** 搖桿讀的是位置不是按下與否，所以手把那兩組只列
  按鍵類的功能。鍵盤則連搖桿方向都可以綁，因為一個鍵只能是一個方向。

## 存在哪裡、怎麼傳給遊戲

`settings.ini`：

```ini
player1_device=both
player2_device=gamepad
player1_gamepad=PlayStation controller
player2_gamepad=
player1_keys=a=Z,b=X,start=Enter,...
player1_pad=a=a,b=b,...
player2_keys=...
player2_pad=...
```

環境變數：`SFR_PLAYER1_INPUT`、`SFR_PLAYER2_INPUT`、`SFR_PLAYER1_GAMEPAD`、
`SFR_PLAYER2_GAMEPAD`、`SFR_PLAYER1_KEYS`、`SFR_PLAYER1_PAD`、`SFR_PLAYER2_KEYS`、
`SFR_PLAYER2_PAD`。**沒有設就是預設值**，
所以一個在這個分頁出現以前寫下的 `settings.ini` 玩起來跟以前一模一樣。

鍵是用 Windows 的虛擬鍵碼記的，寫檔時盡量寫成名字（`Z`、`Enter`、`Numpad 8`），
認不得的就寫數字。Linux／Android 的 SDL 版會把這些碼翻成自己的 scancode，所以
一份設定檔在哪個平台都是同一個鍵。

實作在 [`input_bindings.h`](../src/input_bindings.h)；遊戲端在
`NativeInput::set_player`，啟動器端在 `launcher_main.cpp` 的 `controls_settings`。

## 用手把操作啟動器

整個啟動器都能只用手把操作：

- **LB／RB** 切換設定分頁，切過去時第一個選項會亮起。
- **十字鍵／左類比** 在選項之間移動；設定面板和下方的按鈕是同一頁，往上往下就能進出面板。
- **A** 按下按鈕、切換開關、打開下拉選單（選單裡用十字鍵選，A 確定）。
- **滑桿**（音量、比賽畫面更新間隔）選到後直接用 **左右** 調整，不必先按 A。
- **B** 離開，**START（☰）** 開始遊戲。

例外：「瀏覽…」會打開 Windows 的檔案對話框，那個視窗無法用手把操作；路徑也可以直接打字。

測試用（`launcher_main.cpp`）：`SFR_LAUNCHER_INPUT_SCRIPT="rb@3,down@4,a@5"` 以腳本手把操作
（`按鍵@秒[+持續秒數]`，秒數以 60 格計），`SFR_LAUNCHER_SHOTS=<資料夾>` 與 `SFR_LAUNCHER_SHOT_EVERY=<格數>`（Windows）
定期存截圖，`SFR_LAUNCHER_NAV_TRACE=1` 在 stderr 記錄焦點落在哪個項目。
