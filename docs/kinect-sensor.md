# 實體 Kinect

日期：2026-09-27。接續 [攝影機體感輸入](camera-input.md)。

遊戲原本就是 Kinect 專用的：Xbox 360 上的 NUI 函式庫回報玩家骨架，遊戲自己的
約 25 個手勢判斷器（跳躍、蹲下、踢地、傾斜……）讀這副骨架。沒有感測器時，
[NUI 模擬](main-menu.md) 用手把假裝出一副骨架，[比賽](race-controls.md) 更直接
把判斷器換成讀手把。

**Kinect for Windows 執行階段（SDK 1.8，`Kinect10.dll`）就是同一套 NUI 函式庫
搬到 PC 上**：骨架框架一樣是六副、每副 20 個關節、同一個攝影機座標系（公尺，
+x 朝玩家的右手邊，+z 離開感測器）。所以感測器看到的東西幾乎原封不動交給遊戲，
比賽也改回由遊戲自己的判斷器讀真實身體，而不是手把。

## 怎麼用（Xbox 360 版 Kinect）

1. **先不要插 Kinect**。下載並安裝
   [Kinect for Windows SDK 1.8](https://www.microsoft.com/download/details.aspx?id=40278)
   （`KinectSDK-v1.8-Setup.exe`，含驅動程式與執行階段）。
   - Xbox 360 版**必須裝完整 SDK**；只裝 *Kinect for Windows Runtime 1.8* 時，
     執行階段會拒絕 Xbox 360 版感測器（那個 Runtime 只給 Kinect for Windows 感測器用）。
   - Kinect for Windows 第一代感測器則只裝 Runtime 就夠了。
2. Kinect 接上**電源轉接線**，轉接線的電源插上插座，USB 插到電腦
   （建議直接插主機板上的 USB 2.0／3.0 孔，不要經過 USB Hub）。
   第一次插上時 Windows 會安裝驅動，裝置管理員裡會出現「Kinect for Windows」
   底下的 Audio、Camera、Device 等項目。綠燈亮起表示有電。
3. （選用）SDK 附的 *Developer Toolkit Browser* 裡的 *Skeleton Basics* 範例可以先確認
   骨架追蹤正常。
4. launcher → 進階 →「攝影機」選 **Kinect**，按「測試」：
   - 「Kinect 運作中」：可以玩了。
   - 「尚未安裝 Kinect SDK 1.8」：按「下載 SDK」回到步驟 1。
   - 「沒有連接 Kinect」：檢查電源轉接線與 USB。
   - 「找到 Kinect，但無法啟動」：關掉其他使用 Kinect 的程式（SDK 範例、Kinect Studio），
     或 Xbox 360 版只裝了 Runtime。
5. 站在感測器前約 1.5～3 公尺、全身入鏡，開始遊戲。log 會有 `NATIVE_KINECT started=1`，
   之後每五秒一行 `NATIVE_KINECT frames=… with_body=… bodies=…`。

## 感應器位置（側放、斜放）

這是滑板遊戲，玩家本來就**側身**對著電視。原本的感應器在電視旁，看到的是玩家的
側面：後面那隻手和腳被身體擋住。launcher 的「感應器位置」可以把 Kinect 放到玩家
周圍 8 個方位之一（俯視圖點選；`SFR_KINECT_PLACEMENT`）：

| 值 | 位置（以面向螢幕為準） | 角度 |
| --- | --- | --- |
| `front` | 正前方（電視旁，原作的位置） | 0° |
| `front-right` / `right` / `behind-right` | 右前方／右側／右後方 | 45° / 90° / 135° |
| `behind` | 正後方 | 180° |
| `behind-left` / `left` / `front-left` | 左後方／左側／左前方 | 225° / 270° / 315° |

- **放在胸口朝向的那一側**：Regular（左腳在前）胸口朝右，Goofy 朝左。從那一側看
  得到全身，Kinect 的追蹤引擎也假設人是面對它的。
- **斜角比正側邊好**：感應器在正側邊時，往背後那一側伸出的手會被身體擋住；斜角
  一樣看得到胸口，也看得到繞到背後的手。朝螢幕方向投擲的手，在側邊與斜角的感應器
  看來都是橫向移動，比在電視前面（直直朝感應器過來、只能靠深度）更好辨識。
- 房間寬但到電視很淺時，放在側邊或斜前方，就能用房間的長邊拉出全身入鏡的距離。
- 正後方看到的也是側面（和正前方一樣）：從正後方看 Regular 玩家，就等於從正前方看
  Goofy 玩家（整個場景轉 180°，是旋轉不是鏡像），原作本來就支援，所以不用對調左右。

### 切換站姿（switch）

遊戲中切換 Regular／Goofy 是**轉身**：前後腳對調、胸口轉到另一邊。這時側邊或斜角的
感應器就從看到胸口變成看到背部，而 Kinect 的追蹤引擎假設人面對它，看到背部時會把
左手叫成右手。

判斷看的是**腳**：Kinect 的腳（foot）關節在腳踝前方、腳尖那一側，而腳尖朝向胸口。
換算到正前方座標後，兩隻腳「腳 − 腳踝」的平均指向房間右邊（胸口朝右）或左邊；
和感應器所在的那一側比對，胸口背對感應器時就把左右成對換回來。兩隻腳一起算，
所以和追蹤引擎有沒有標反無關（位置是量到的，不是猜的）。要連續 8 格（約 0.3 秒）
都這樣判斷、且腳尖至少朝前 2 公分，才會切換，避免抖動。正前方與正後方看到的都是
側面，不做這個判斷。

比賽中腳常常在動：**踢地加速（kick boost）**是把腳抬起再放回（抬高膝蓋比較好辨識），
玩家也常**跺腳、用腳划地**。所以：

- 一隻腳踝比另一隻高 10 公分以上時，那隻腳視為抬起，只用踩在地上的那隻腳判斷。
- **轉身一定會讓兩肩經過橫跨螢幕的角度**（面對螢幕或背對螢幕），划地、跺腳、踢地
  加速時上半身都維持側身。所以判斷過一次之後，只有在最近 1.5 秒內兩肩展開過
  （橫跨螢幕的分量至少 65%）才允許換邊，而且兩肩展開時先前累積的判斷歸零，轉身後
  要重新連續 8 格一致。兩肩的方向是用量到的位置算的，和追蹤引擎有沒有標反無關。

實測時看 log 的 `NATIVE_KINECT sees=back` / `sees=chest`，或用
`sfr_kinect_probe 30 right`（第二個參數是位置）即時看每個人被判斷成哪一面。
這是依骨架結構推理的做法，**還沒在實機上驗證**；Kinect 第一代的腳關節比較不穩，
若判斷常常跳動，可以調高門檻或連續格數。

換算（[`KinectPlacementTransform`](../src/kinect_sensor.h)）：感應器在角度 a
（從螢幕順時針）時，它的視線是正前方空間的 (−sin a, 0, cos a)，它的左邊是
(cos a, 0, sin a)，所以 x' = x·cos a − z·sin a、z' = x·sin a + z·cos a。
第一次看到一個人時記下他的位置當基準點，之後都從這一點換算，並把他放到正前方
2.5 公尺（模擬玩家的位置）；基準點固定，所以往哪邊走、往哪邊傾，遊戲都看得到。
轉過的框架不帶感應器的地板平面（那是感應器自己的座標），由模擬的地板代替。
正前方時框架完全不動。四個正方位與斜角都有單元測試。

## Kinect v2（Xbox One 版）

1. 需要 **Kinect v2**（Xbox One 版需另購 *Kinect Adapter for Windows*，副廠也可）與
   電腦上的 **USB 3.0** 連接埠（Intel 或 Renesas 晶片的控制器較穩，部分 AMD 控制器有相容性問題）。
2. 安裝 [Kinect for Windows SDK 2.0](https://www.microsoft.com/download/details.aspx?id=44561)，
   可以先用 SDK Browser 裡的 *Body Basics* 確認骨架正常。
3. launcher 一樣選 **Kinect**：先找 Xbox 360 版，找不到再找 v2，會用第一台找到的。
   log 會寫 `NATIVE_KINECT started=1 model=Kinect v2`。

v2 有 25 個關節，前 20 個與第一代同名同順序，只有一處不同：第一代的「肩中心」
對應 v2 的 SpineShoulder（20），而不是位置更高的 Neck（2）；指尖與拇指（21～24）不用。
身體中心用 SpineBase。座標系與第一代相同（公尺、+x 朝玩家右手、+z 離開感測器），
所以不用換算。v2 SDK 沒有第一代的 `NuiTransformSmooth`，關節直接交給遊戲；
v2 本身比第一代穩定，實測若覺得抖再加平滑。

程式在 [`kinect_v2_win32.cpp`](../src/kinect_v2_win32.cpp)：執行時才載入 `Kinect20.dll`，
COM 介面依 SDK 2.0 的 `Kinect.h` 宣告到用得到的最後一個方法為止（vtable 只看順序）。
關節對應與「兩種感測器都失敗時回報哪個原因」在 [`kinect_sensor.cpp`](../src/kinect_sensor.cpp)，
有單元測試。

不開遊戲也可以檢查：`sfr_kinect_probe [秒數]` 會印出追蹤到的人與右手位置。
每 5 秒另有一行 `NATIVE_KINECT model=v1 frames=… with_body=… empty_waits=… failed=…`：
`frames=0` 且 `failed` 一直增加（`last_error=0x83010001`，`E_NUI_FRAME_NO_DATA`）表示
感測器有通知新畫面、卻交不出骨架。一台 Kinect for Xbox 360（SDK 1.8、Windows 10）
在「事件觸發就讀」時每格都如此，改成讓 `NuiSkeletonGetNextFrame` 自己等待後每秒
約 28 格，所以預設是後者；`SFR_KINECT_V1_READ=event` 可切回，`SFR_KINECT_V1_FLAGS`
可再加 `NuiInitialize` 旗標（例如 `0x1` 深度＋玩家索引）試別的感測器。

找不到感測器或執行階段時（`NATIVE_KINECT started=0 reason=…`），遊戲會退回手把
模擬，不會卡住。`reason` 可能是：

| reason | 意思 |
| --- | --- |
| `no-runtime` | 沒有 `Kinect10.dll`：還沒裝 SDK／Runtime |
| `no-sensor` | 執行階段看不到感測器：沒插、沒接電源，或驅動程式沒裝好 |
| `initialize-0x…` | 感測器在但無法啟動：被別的程式佔用，或 Xbox 360 版只裝了 Runtime |
| `unsupported-platform` | 非 Windows（見下方） |

## 做了什麼

- [`kinect_sensor_win32.cpp`](../src/kinect_sensor_win32.cpp)：執行時才 `LoadLibrary`
  載入 `Kinect10.dll`（建置不需要 SDK，沒有 Kinect 的人也不需要那個 DLL），
  開啟骨架追蹤，在自己的執行緒上讓 `NuiSkeletonGetNextFrame` 等待新框架，套用 SDK
  建議的 `NuiTransformSmooth` 平滑參數（約一格延遲）。
- 感測器仰角（[`kinect_level`](../src/kinect_sensor.h)）：骨架座標以感測器為準，放得低、
  鏡頭往上仰時，站直的人看起來往後倒，遊戲的站姿校正就量到歪的站姿。每格依感測器加速度計
  給的重力方向，把骨架和地板繞感測器轉正，使重力朝正下方（超過 30 度或沒有重力資料就不動；
  `SFR_KINECT_LEVEL=0` 關閉）。`sfr_kinect_probe` 與遊戲的 `NATIVE_KINECT_BODY`（每 3 秒）
  都會印出 `level=`（轉掉的度數），`NATIVE_KINECT_BODY` 另有髖、肩、頭、雙手的位置與雙手的
  追蹤狀態（2 追蹤到、1 推測、0 沒有）。
- 比賽轉向（[`nui_race_hooks.cpp`](../src/nui_race_hooks.cpp)）：遊戲的傾斜判斷讀身體紀錄
  +640／+644，在主機上是深度影像裡玩家左右兩側的像素數，不是關節。這裡沒有深度影像送進
  遊戲，所以 1P 的傾斜改由骨架算：沿用 webcam 體感的做法，比賽開始先保持站姿約 0.6 秒
  校正，之後以肩膀相對髖部的左右傾角換成那一對數值（`NUI_RACE_SENSOR_LEAN active=1`，
  `CAMERA_RACE_CALIBRATION ready=1`；`SFR_CAMERA_RACE_TRACE=1` 每 30 格印出 `lean=`）。
  傾角約 4° 起算、30° 算傾到底（`lean=±1`）；傾到底時寫入遊戲可讀的最大值 3.5（兩數相差
  4.5 倍），`SFR_KINECT_LEAN_SCALE`（0.1–3.5）可調小。原本傾到底只給 1，彎到極限也只轉一點點。
  其他動作仍由遊戲自己的判斷器讀骨架，但**蹲下**（連帶蓄力與起跳）原版判斷器從來不成立，
  它要的應該是主機上深度影像給的資料。所以原版判斷「沒有」時，改看 1P 骨架：髖部比校正時的
  站姿低一段距離持續 0.12 秒算蹲下，再明顯站起來算起跳（同 webcam 體感）。感應器放高時常
  看不到腳，猜出來的腳踝會讓「髖部離腳踝多高」忽大忽小，所以 Kinect 改量髖部比校正時低了
  多少（地板在校正時固定）：預設降 0.15 公尺算蹲下，`SFR_KINECT_CROUCH_DEPTH`（0.05–0.35）可調。
  `SFR_KINECT_BODY_GESTURES=0` 關掉；`SFR_CAMERA_RACE_TRACE=1` 時每次結果改變印出
  `KINECT_GESTURE address= original= result=`（822C8778 蹲、822CB840 蓄力、822C9050 跳、
  822CA6B0 Side）。2P 的傾斜與蹲跳還沒接。
- [`KinectPlayerSlots`](../src/kinect_sensor.h)：感測器最多完整追蹤兩個人。
  每個人只要還被追蹤就留在原本的欄位（遊戲以欄位與 tracking id 1／2 認人，
  第 1 位是登入的設定檔），新走進來的人補空的欄位。
- [`nui_hooks.cpp`](../src/nui_hooks.cpp) 的 `NuiSkeletonGetNextFrame`：
  `SFR_CAMERA=kinect` 時，框架表頭的地板平面與重力方向用感測器的，兩個欄位寫入
  真實的關節、每個關節的追蹤狀態（追蹤／推測）與身體中心。沒有人站在前面就是空框架，
  遊戲會像在主機上一樣請玩家站到感測器前。
- [`nui_race_hooks.cpp`](../src/nui_race_hooks.cpp)：有實體感測器時
  （`nui_body_from_sensor()`），比賽**不**換掉身體紀錄、**不**覆寫手勢判斷器，
  也**不**跳過「On your Gear!」的身體量測——全部交還給遊戲原本的程式。
- **語音指令**：launcher 的「語音指令」（`SFR_VOICE=1`，Windows）用 Windows 語音辨識（SAPI）
  聽預設麥克風。Kinect 的麥克風陣列在 Windows 上就是一個錄音裝置，設成預設就會用它。
  [`voice_commands.cpp`](../src/voice_commands.cpp) 列出聽的詞：遊戲詞彙的 start、ok、back、
  next、up／down／left／right、pause（遊戲的 `pauseopen`，比賽中說 start 也是）、restart、
  replay、main menu，以及繁體中文的開始、好／確定、返回、下一步、往上…、暫停、重來、重播、
  主選單。只接受辨識器有一般以上信心的結果，免得雜音變成指令；聽到的詞像按鍵一樣寫進
  遊戲的語音欄位（input+5440），手把按鍵照樣可用。`SFR_VOICE_LANGUAGE` 可指定辨識器語言
  （409 英文、404 繁中），預設用系統的；辨識器說不出的詞（例如中文辨識器遇到英文）會被略過。
  log：`NATIVE_VOICE started phrases=…`、`NUI_VOICE heard=… word=…`。尚未實機測試。

## 其他平台

- **Linux**：Kinect 第一代沒有骨架追蹤器可用，但核心的 `gspca_kinect` 驅動會把
  它的彩色鏡頭變成一般的 `/dev/videoN`，可以用「體感」（webcam）模式讀它。

## 還沒驗證的

這台開發機沒有 Kinect，也沒有遊戲光碟：資料結構大小由編譯期檢查確認
（`NUI_SKELETON_DATA` 436 bytes、`NUI_SKELETON_FRAME` 2664 bytes、v2 `Joint` 20 bytes），
欄位分配、v2 關節對應與框架寫入有單元測試（`kinect_sensor` CTest），Windows 版以
mingw 編譯連結過。Kinect for Xbox 360（v1）已在實機上讀到骨架畫面與地板；v2 還沒在
實機上試過。v2 的 COM 方法順序若與 SDK
不符，`sfr_kinect_probe` 會讀不到骨架或當掉，那是最先要看的地方。
特別需要確認的：

- 選單的手部游標位置是否對得上（模擬玩家的右手在 x=0.175 為畫面中央）。
- 比賽中遊戲自己的判斷器在真實骨架上的反應，以及「On your Gear!」量測能否完成。

## 深度串流（進行中）

主機上遊戲除了骨架，還讀 Kinect 的深度影像加玩家遮罩（每像素 16 位元：高 13 位元深度、
低 3 位元玩家編號，320×240，與 SDK 1.8 的 `NUI_IMAGE_TYPE_DEPTH_AND_PLAYER_INDEX` 相同，
只差位元組順序）。深度檢視物件（Kinect 管理器 +7736，建構在 `82437F38`）開啟串流
`NuiImageStreamOpen`（`82768C40`，類型 0、解析度 1），建立 4 張材質與 +72 的緩衝區；
深度檢視（`82439530`）再對每位玩家跑 `82439998`、`82439D80`，算出輪廓資料，比賽的傾斜
（+640／+644）等由此而來。函式庫沒有初始化時開啟會失敗，建構函式就丟掉物件，所以之前
一直沒有深度資料（`NUI_DEPTH_VIEW_SKIPPED`）。

`SFR_KINECT_DEPTH=1`（且 `SFR_CAMERA=kinect`）讓開啟成功（`NUI_IMAGE_STREAM_OPEN ... backend=kinect`），
物件建立後印出一次 `NUI_DEPTH_VIEW object= vtable= fetch=`；取幀（`fetch`）與真實深度資料
是下一步。
