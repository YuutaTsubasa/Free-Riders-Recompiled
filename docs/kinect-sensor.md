# 實體 Kinect

日期：2026-09-27。接續 [攝影機體感輸入](camera-input.md)。

遊戲原本就是 Kinect 專用的：Xbox 360 上的 NUI 函式庫回報玩家骨架，遊戲自己的
約 30 個手勢判斷器（跳躍、蹲下、踢地、傾斜……）讀這副骨架（遊戲先用函式庫的 `NuiTransformSmooth` 平滑它），
傾斜、蹲下、跳躍與賽前量測另外讀深度影像（見「遊戲怎麼用骨架」）。沒有感測器時，
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
4. launcher → 體感與攝影機 →「攝影機」選 **Kinect（Xbox 360）**，按「開啟預覽」：
   - 「Kinect 運作中」：可以玩了。
   - 「尚未安裝 Kinect SDK 1.8」：按「下載 SDK」回到步驟 1。
   - 「沒有連接 Kinect」：檢查電源轉接線與 USB。
   - 「找到 Kinect，但無法啟動」：關掉其他使用 Kinect 的程式（SDK 範例、Kinect Studio），
     或 Xbox 360 版只裝了 Runtime。
5. 站在感測器前約 1.5～3 公尺、全身入鏡，開始遊戲。log 會有 `NATIVE_KINECT started=1`，
   之後每五秒一行 `NATIVE_KINECT frames=… with_body=… bodies=…`。

## 感應器位置

感應器放在電視旁、玩家正前方，就是主機上的擺法；遊戲收到的骨架不做任何轉換。
側放、斜放的換算先移到 `claude/kinect-placement` 分支，之後再做。

## Kinect v2（Xbox One 版）

v2 一律把自己的深度換成 Kinect v1 格式的深度加玩家編號影像交給遊戲（見下方「Kinect v2 深度」），
遊戲的深度檢視與判斷器照 v1 的方式使用它；沒有開關，也沒有主機端的替代判定：
跟主機一樣，比賽的輸入永遠來自遊戲自己的判斷器。它們讀**深度與骨架兩者**：深度只用在傾斜那一對數值、蹲下、跳躍與賽前量測，
其他約 20 個判斷器（舉手、側移、煞車、站姿、踢地、招式、道具、抓取、特技、加速、蓄力……）與轉向的身體傾角讀的是骨架關節（見下方「遊戲怎麼用骨架」）；
選單的游標與玩家身分也是骨架。
深度讀取器打不開時，遊戲就拿不到深度（log 會寫一行 `NATIVE_KINECT_DEPTH unavailable model=… reason=…`，
launcher 的 Kinect 預覽也會顯示原因），不會有替代資料。
launcher 的預覽視窗（與遊戲中的「Kinect 調整」視窗）會顯示 v2 的彩色與深度影像並疊上骨架（見下方「預覽視窗」）。
遊戲最多接收兩位玩家，追蹤身分與原始感測器槽位保持穩定。
這條路徑有自動測試，尚未完成 Kinect v2 實機遊玩驗證。
v2 的資料流程、#97／#132／#165 的 log 分析、待實機確認的項目，以及啟動器「體感追蹤紀錄」
開關（`SFR_CAMERA_RACE_TRACE=1`，只寫數字、不含影像）的用法，見 [kinect-v2.md](kinect-v2.md)（英文）。

1. 需要 **Kinect v2**（Xbox One 版需另購 *Kinect Adapter for Windows*，副廠也可）與
   電腦上的 **USB 3.0** 連接埠（Intel 或 Renesas 晶片的控制器較穩，部分 AMD 控制器有相容性問題）。
2. 安裝 [Kinect for Windows SDK 2.0](https://www.microsoft.com/download/details.aspx?id=44561)，
   可以先用 SDK Browser 裡的 *Body Basics* 確認骨架正常。
3. launcher 選 **Kinect v2（Xbox One）**：只開 v2，不會去找 Xbox 360 版，所以兩種都接上時也能指定 v2
   （launcher 以 `SFR_CAMERA=kinect` 加 `SFR_KINECT_MODEL=2` 交給遊戲）。log 會寫
   `NATIVE_KINECT started=1 model=Kinect v2`。
   舊的 `camera=kinect`（`SFR_KINECT_MODEL=1` 或沒有設定）仍先找 Xbox 360 版；找不到、但 v2 開得起來時
   改用 v2，並寫一行 `NATIVE_KINECT no Kinect (Xbox 360) opened, using the Kinect v2 found instead`，
   讓只有 v2 的舊設定繼續能用。
4. launcher 的「深度視角」（`kinect_v2_view`，見下方「Kinect v2 深度」的三種視角）依序是 **Recompiled**（`ideal`，launcher 的預設）、
   **模擬**（`fit`）、**原始**（`sensor`）。選 Recompiled 時下面有距離、高度、跟隨速度、死區、骨架距離五個滑桿，
   再下面是「俯視位置」圖（見「預覽視窗」的俯視圖；預覽沒開時只畫框與兩台相機，並提示「開啟預覽即可看到玩家位置」）。
   舊 `settings.ini` 已存的視角照舊；**遊戲本身**沒有設 `SFR_KINECT_V2_DEPTH_VIEW` 時仍是 `fit`，只有 launcher 的預設改成 Recompiled。

v2 有 25 個關節，前 20 個與第一代同名同順序，只有一處不同：第一代的「肩中心」
對應 v2 的 SpineShoulder（20），而不是位置更高的 Neck（2）；指尖與拇指（21～24）不用。
身體中心用 SpineBase。座標系與第一代相同（公尺、+x 朝玩家右手、+z 離開感測器），
所以不用換算。v2 SDK 沒有第一代的 `NuiTransformSmooth`，但**遊戲自己會平滑骨架**（見下方「骨架平滑」）：
v1 與 v2 的關節都照原樣交給遊戲，由遊戲的 `NuiTransformSmooth`（現在以原生方式實作）平滑一次。
v0.7.2 起主機端對 v2 的平滑（`KinectJointSmoother`，當時是為了比賽中關節會抖，#97）只是「碰巧」補上了遊戲沒做成的平滑，
現在預設關閉（`NATIVE_KINECT model=v2 smooth=0`），否則同一副骨架會被平滑兩次。

launcher 的「平滑 Kinect v2 追蹤」已移除；舊 `settings.ini` 裡的 `kinect_smooth` 會被忽略，也不會再寫回，launcher 不再傳 `SFR_KINECT_SMOOTH`。
「選單游標穩定」與預覽視窗兩種都有。傾身靈敏度、蹲下深度與預覽的「校正」都已移除（遊戲的深度判斷器自己決定這些）。舊 `settings.ini` 裡的 `kinect_lean`、`kinect_crouch_cm`、`kinect_v2_depth` 會被忽略，也不會再寫回。

程式在 [`kinect_v2_win32.cpp`](../src/kinect_v2_win32.cpp)：執行時才載入 `Kinect20.dll`，
COM 介面依 SDK 2.0 的 `Kinect.h` 宣告到用得到的最後一個方法為止（vtable 只看順序）。
關節對應與「兩種感測器都失敗時回報哪個原因」在 [`kinect_sensor.cpp`](../src/kinect_sensor.cpp)，
有單元測試。

不開遊戲也可以檢查：`sfr_kinect_probe [秒數]` 會印出追蹤到的人與右手位置（加 `--smooth` 時先用和遊戲相同的濾波平滑，等於 `SFR_KINECT_SMOOTH=1`）；
`sfr_kinect_probe [秒數] --images [檔名前綴]` 另外把第一張彩色與深度影像（與預覽相同的影像大小：v1 為 640×480／320×240，v2 依畫面長寬比）
存成 `kinect_colour.bmp`、`kinect_depth.bmp`（玩家像素為紅色），並印出頭部落在兩張影像的哪個位置。
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

感測器啟動後若超過 500 ms 沒有新骨架，會清除追蹤與玩家身分，不會持續送出凍結姿勢。
此時仍保留實體感測器模式；收到新框架後恢復追蹤，不會自動改送手把模擬骨架。

## 做了什麼

- [`kinect_sensor_win32.cpp`](../src/kinect_sensor_win32.cpp)：執行時才 `LoadLibrary`
  載入 `Kinect10.dll`（建置不需要 SDK，沒有 Kinect 的人也不需要那個 DLL），
  開啟骨架追蹤，在自己的執行緒上讓 `NuiSkeletonGetNextFrame` 等待新框架，原樣交出（不平滑：遊戲自己平滑，
  見「骨架平滑」；`SFR_KINECT_SMOOTH=1` 才在這裡用 SDK 的 `NuiTransformSmooth`）。
- `NATIVE_KINECT_BODY`（每 3 秒）印出 `gravity_y=`、髖、肩、頭、雙手的位置與雙手的追蹤狀態
  （2 追蹤到、1 推測、0 沒有）。骨架座標以感測器為準，原樣交給遊戲，不做水平校正。
- 比賽轉向（[`nui_race_hooks.cpp`](../src/nui_race_hooks.cpp)）：遊戲的傾斜判斷讀身體紀錄
  +640／+644，在主機上是深度影像裡玩家左右兩側的像素數，不是關節；但轉向還另外混入一個骨架的傾角
  （`822C6200` 的 head − spine 傾角、`822C8958` 的肩中心 − 髖中心側傾）。Kinect（v1 與 v2）的比賽**一律**由遊戲自己的判斷器決定
  （深度用於傾斜、蹲下、跳躍；蓄力、側移與其他約 20 個判斷器讀骨架關節），主機端沒有替代或補充判定，
  也沒有開關（已移除 `SFR_KINECT_DEPTH`、`SFR_KINECT_BODY_LEAN`、`SFR_KINECT_BODY_GESTURES`、
  `SFR_KINECT_LEVEL`）。深度影像超過 0.5 秒沒到遊戲時，只在影像恢復時記一行
  `NUI_KINECT_DEPTH gap ms=…`，行為不變。webcam 的「體感」另有自己的動作判斷
  （[camera-input.md](camera-input.md)），不受影響。
- [`KinectPlayerSlots`](../src/kinect_sensor.h)：遊戲最多接收兩副完整骨架。
  每個人只要還被追蹤就保留其身分與感測器槽位，新走進來的人補上離開者的位置。
  這是硬性限制：遊戲（`82438EB0`）把**每一個** TRACKED 槽位依序寫進管理器 +7740 起只有兩格的陣列，第 3 副會寫到 +7748，
  第 4 副會蓋掉骨架執行緒的停止旗標（+7752）。單元測試（`kinect_sensor`）讓 v2 一直有 3–6 個人進出、換位置、順序打亂，
  每一格都確認交給遊戲的骨架（寫進遊戲框架後的 TRACKED 數）不超過兩副、每人一次、各在自己的槽位，而且還在場的玩家不會被換掉。
  這個測試找到一個漏洞：感測器在同一格給出兩個相同的 tracking id 時（v2 的 64 位元 id 縮成遊戲用的 32 位元），兩個都會留下，
  加上另一位玩家就是三副；現在每個玩家只留第一個。
- [`nui_hooks.cpp`](../src/nui_hooks.cpp) 的 `NuiSkeletonGetNextFrame`：
  `SFR_CAMERA=kinect` 時，框架表頭的地板平面與重力方向用感測器的，兩個欄位寫入
  真實的關節、每個關節的追蹤狀態（追蹤／推測）與身體中心。沒有人站在前面就是空框架，
  遊戲會像在主機上一樣請玩家站到感測器前。
- [`nui_race_hooks.cpp`](../src/nui_race_hooks.cpp)：有實體感測器時
  （`nui_body_from_sensor()`），比賽保留原生身體紀錄與「On your Gear!」身體量測，原版深度判定照常。
- **Pause Gesture／暫停手勢**：Xbox 360 Kinect 的系統暫停手勢（universal pause）：右手垂在身側、
  左手伸直往身體外側斜下約 45 度舉著。主機上這是系統的功能，不是遊戲的：舉著時左下角出現進度圖示，
  然後叫出遊戲的暫停選單。這裡由 [`pause_gesture.h`](../src/pause_gesture.h) 判斷姿勢，舉滿
  `SFR_PAUSE_GESTURE_SECONDS` 秒（預設 3）後，在下一次輸入更新送出
  比賽中的 START，也就是遊戲聽到的 `pauseopen`，和語音說「暫停」走同一條路。
  條件：左臂接近伸直（肩到手至少是上臂加前臂的 85%）、左手在肩膀外側下方 25 到 65 度、
  朝旁邊而不是朝感應器伸，而且右手垂在右肩下方、離正下方 30 度以內。Kinect 追蹤到的任何人都可以
  觸發，webcam 則是畫面中的玩家。進度是「充能」：姿勢符合時增加，不符合時以同樣速度往回減少（不歸零），
  所以短暫的追蹤中斷或晃動只會扣掉那一下的時間；兩次更新之間的停頓最多算 0.1 秒。觸發一次後要先放下手臂才能再觸發。
  舉著的時候畫面左下角會出現圓形進度環（中間是一個做這個姿勢的光滑人偶，像照鏡子一樣一手往畫面左邊斜下伸出、另一手垂在右側），從正上方順時針填滿，滿了就暫停（舉超過 0.3 秒才出現，
  免得比賽中手臂剛好經過這個姿勢時閃一下；只在比賽中顯示）。進度環的樣式是這裡自己畫的，
  不是主機的原圖，也沒有主機的提示音。它由畫面最後輸出的 blit 著色器畫
  （[`blit.hlsl`](../src/shaders/blit.hlsl)），借用觸控按鈕 START 的那一格，顯示時那一格的觸控按鈕暫時不畫。
  只在比賽（或重播）中、而且沒有選單開著時作用：暫停選單本身（比賽旗標仍開著，以轉盤選單最近 0.5 秒有沒有運作判斷）、
  選單頁面、對話框、劇情畫面、結算畫面都不作用，進度環也不顯示，免得在暫停選單裡再做一次手勢被當成 START 而直接繼續比賽。`SFR_PAUSE_GESTURE=0` 關閉；
  `SFR_PAUSE_GESTURE_SECONDS` 範圍 0.5 到 10。log：`NUI_PAUSE_GESTURE racing=1 pause`。尚未實機測試。
- **Kinect v2 的暫停手勢（Stop Sign）**：只有接 Kinect v2 時有效（v1 與 webcam 讀不到手掌）。一隻手**張開、手指朝上、
  手心朝向感應器**，像比「停」一樣舉在臉前（肩膀高度到頭頂上方一點、離身體中線 0.35 公尺內、比肩膀往前至少 0.1 公尺），
  另一隻手放低在肩膀以下（雙手都舉起是比賽動作，不算），撐滿同樣的秒數就暫停，用同一個進度環。手掌張開是 v2 的
  手部狀態（open），手心朝向由 v2 追蹤的手掌、指尖、拇指三點算出（與感應器方向夾角 60 度以內才算）。原本的 45 度
  姿勢在 v2 上照樣可用；只在比賽中作用（選單裡舉起張開的手是游標）。舉起張開的手時每秒記一行
  `NUI_STOP_SIGN hand= palm_z= posed=`（palm_z 是 −1 正對感應器到 1 背對），用來在實機上確認手心朝向的判斷。尚未實機測試。
- **語音指令**：launcher 的「語音指令」（`SFR_VOICE=1`，Windows）用 Windows 語音辨識（SAPI）
  聽預設麥克風。Kinect 的麥克風陣列在 Windows 上就是一個錄音裝置，設成預設就會用它。
  [`voice_commands.cpp`](../src/voice_commands.cpp) 列出聽的詞：遊戲詞彙的 start、ok、back、
  next、up／down／left／right、pause（遊戲的 `pauseopen`，比賽中說 start 也是）、restart、
  replay、main menu，以及繁體中文的開始、好／確定、返回、下一步、往上…、暫停、重來、重播、
  主選單。只接受辨識器有一般以上信心的結果，免得雜音變成指令；聽到的詞像按鍵一樣寫進
  遊戲的語音欄位（input+5440），手把按鍵照樣可用。`SFR_VOICE_LANGUAGE` 可指定辨識器語言
  （409 英文、404 繁中），預設用系統的；辨識器說不出的詞（例如中文辨識器遇到英文）會被略過。
  log：`NATIVE_VOICE started phrases=…`、`NUI_VOICE heard=… word=…`。尚未實機測試。

## 錄製（離線分析用）

把感測器的每一格骨架與比賽中遊戲判斷器的回答存成 CSV，之後不開遊戲、不接感測器也能分析
（[`kinect_record.h`](../src/kinect_record.h)、[`kinect_race_record.h`](../src/kinect_race_record.h)，有單元測試 `kinect_record`）。
v1 與 v2 用同一種欄位格式。

- **遊戲中**：設 `SFR_KINECT_RECORD=<前綴>`（例如 `D:\rec\run1`）。
  - `<前綴>-skeleton.csv`：感測器每一格一列（每個追蹤到的人一列；沒人時也有一列，`bodies=0`），
    在平滑與水平校正**之前**原樣記下：主機 `steady_clock` 微秒（`steady_us`）、感測器自己的編號與時間戳
    （v1 `dwFrameNumber`／`liTimeStamp` 毫秒；v2 為後端計數／`RelativeTime` 100 ns）、地板平面、重力（v1）、
    深度影像無效像素比例（`depth_invalid`，有開深度時）、骨架槽位、完整 tracking id、v1 的骨架中心、v2 的手部狀態，
    以及 25 個關節（v2 `JointType` 的順序與名稱）的位置與狀態（0 未追蹤、1 推測、2 追蹤；v1 沒有的 Neck 與指尖、拇指留空，
    v1 的 ShoulderCenter 寫在 `spine_shoulder`），最後一欄 `clipped_edges` 是 v2 的 `IBody::get_ClippedEdges` 位元遮罩
    （1 右、2 左、4 上、8 下，0 無；v1 不提供，留空）。沒有這欄的舊錄製檔照樣讀得進來，可拿它對照比賽不準時玩家是否被畫面切到。
  - `<前綴>-race.csv`：比賽中每次更新、每個由感測器驅動的玩家紀錄一列：遊戲深度檢視的傾斜那一對數值
    （`depth_right`／`depth_left`），以及遊戲原本的蹲 822C8778、蓄力 822CB840、跳 822C9050、側移 822CA6B0
    判斷器這次回答的 `*_original`（遊戲自己的）與 `*_result`（比賽實際採用的，兩者相同，因為沒有任何東西取代它）。
    v1 的 `*_original` 與深度傾斜就是主機版的深度判斷，可當標準答案，拿來和 v2 合成深度的結果比較。
  - `<前綴>-nui.csv`（[`kinect_nui_record.h`](../src/kinect_nui_record.h)）：遊戲自己的骨架消費者這一次做了什麼。
    實體 Kinect（v1／v2）時，每次輸入更新（`824920F8`，每個遊戲畫格一次，選單與比賽都有）每位有身體紀錄的玩家一列（210 欄，有標頭）：
    - `steady_us`、`update`（輸入更新的計數）、`player`（0、1）、`tracking_id`、`body_slot`（rec +784）、`racing`。
    - 兩隻手的選單游標（`left_*`、`right_*`；`82492F30` 剛算完的值，手部封包 P = input + 32 + 2544·玩家 + 16 + 400·手）：
      `flags`（P+0）、`theta`（P+60，度）、`r`（P+148）、`target_x/_y`（P+96/+100，像素）、`cursor_x/_y`（P+80/+84，像素，選單讀的就是這個）。
    - 身體紀錄的關節（`hand_l hand_r shoulder_l shoulder_r shoulder_c head spine hip_c` 各 `_x _y _z`）：遊戲**平滑之後**、而且是它的鏡像座標 (−x, y, −z)。
    - `lean_depth`、`lean_tilt`：比賽中 `822C6200` 兩個濾波器的平均（source +44 深度傾斜、+88 身體傾角）。
    - 研究列出的每一個比賽判斷器（附錄 A 與 3.3 節，共 32 個：`jump crouch charge arms_up catch_lr catch flying accel accel2 side brake grab kick kick2
      power power2 stance trick hammer over_throw under_swing under_throw inflator shake wipe lever handle front_curve swim hold_hands steer steer_alt`），
      各 4 欄：`_result`（回傳值：1 認出、2 相反狀態、0 其他）、`_primary`、`_secondary`（這個判斷器這次在結果 +4／+20 **新加上的**位元，十進位）、
      `_value`（有自己數值的：煞車與抓取的強度 +80、站姿 +72、特技轉角 +56、拉桿 +82、把手 +83、前彎第二筆結果 +81、擦螢幕的 x（detector +76，y 在 `wipe_y`）、
      轉向 +76）。上一列之後沒被問到的判斷器是空的；同一次更新被問兩次時記最後的回傳值與所有位元。`hold_hands` 問的是整隊，記在玩家 0。
    - `calib_step`（`arms_forward`＝`822C65B0`、`body`＝`822C6AD8`）與 `calib_0`…`calib_132`：該玩家的賽前量測在上一列之後完成時，校正物件 `[source+92]`
      的 +0..+132（+112、+116、+120 是整數像素，其他是浮點）；沒有完成時空白。
    判斷器與量測的值在它們的 hook 裡收集，下一次輸入更新時寫成該玩家的一列，所以一列是「上一列之後到這次輸入更新為止」。
    和其他錄製檔一樣經過 `KinectRecordQueue`，遊戲不等硬碟；60 fps、兩位玩家約每秒 120 列（約 60–100 KB）。
  - 每個檔案最多 `SFR_KINECT_RECORD_MB` MB（預設 512），滿了就停並在 log 說一次（`NATIVE_KINECT_RECORD full`）。
  - **錄製不會等硬碟**（[`KinectRecordQueue`](../src/kinect_record.h)）：所有錄製檔（兩個 CSV、深度檔、原始 v2 檔；遊戲與探針都一樣）
    以前都在呼叫者的執行緒上同步 `fwrite`＋`fflush`，遊戲的 hook 因此會被慢的硬碟卡住（hook 擋住時其他客體執行緒也停，畫面就頓一下）。
    現在呼叫者只把一份自己的緩衝區交給有上限的佇列（`SFR_KINECT_RECORD_QUEUE_MB`，預設 256），每個程式一條背景執行緒寫入並在每批之後
    `fflush`（程式被砍掉時只少還在佇列裡的）。佇列滿了就**丟掉**那一項並計數，不等；log 每秒最多一行
    `NATIVE_KINECT_RECORD dropped=N (disk too slow)`，每 5 秒的 `NATIVE_KINECT model=v1|v2` 行多 `record_dropped= record_queued_mb=`。
    被丟掉的深度或原始影像在檔案裡就是影像編號與 `steady_us` 的空缺，工具照一般的空缺處理。結束時每個檔案最多等 10 秒把佇列寫完再關檔。
    檔案格式與「第一列／第一張到時才開檔」不變。離線工具（`sfr_kinect_depth_resynth`）改成佇列滿時等待，不丟。
    單元測試（`kinect_record`）：順序、預算與丟棄計數、解構時寫完、硬碟卡住時呼叫者不等（假的寫入端卡住時最慢一次約 50 µs）。
  - **錄到 SSD**：A: 是同時放遊戲資料的傳統硬碟，錄製每秒十幾 MB 會和遊戲讀資料搶；雙錄製的腳本預設錄到 `C:\SfrRec`（見 KINECT_DUAL_RECORDING.md）。
- **不開遊戲**：`sfr_kinect_probe <秒數> --v1|--v2|--both --record <前綴>`。`--both` 在同一個程式裡同時開 v1 與 v2，
  先印出各自是否開成功（`Kinect v1: open`／`Kinect v2: not open (no-sensor)`），只開成功一台也照樣錄；
  兩台共用同一個 `steady_clock`。錄製時 v2 的深度讀取器也會打開，以記錄 `depth_invalid`。
  兩個 SDK 的執行環境能否在同一個程式裡同時運作，還沒有實機試過。
- **遊戲＋探針同時**：Windows 的 `steady_clock` 是全機共用的（QueryPerformanceCounter），所以可以讓遊戲用 v1 錄
  （有標準答案），另開 `sfr_kinect_probe <秒數> --v2 --record <另一個前綴>` 錄 v2，事後用 `steady_us` 對齊。
- **干擾 A/B**：`sfr_kinect_record_summary <單開>-skeleton.csv <雙開>-skeleton.csv` 比較站著不動時臀部、肩膀中心、雙手的抖動
  （逐格差的 RMS 除以 √2）、關節追蹤比例、深度無效比例與失去追蹤次數，並對照「抖動增加 < 20%、追蹤比例下降 < 5 個百分點、
  深度破洞增加 < 10 個百分點、失去追蹤 0 次」。
- **深度影像**：`SFR_KINECT_RECORD_DEPTH=1`（探針用 `--depth`）另存深度影像，`<前綴>-race.csv` 也多了遊戲深度檢視的全部特徵；
  格式、記憶體配置與工具見下一節。
- **原始 v2 資料**：`SFR_KINECT_RECORD_RAW=1`（探針用 `--raw`）另存 v2 合成每張影像所用的原始深度、身體索引、身體與地板（`<前綴>-v2raw.bin`），
  可以離線用任何視角重做（見下一節「原始 v2 錄製」）。

## 深度錄製與分析工具

目的：判斷 Kinect v2 合成的 v1 格式深度（`fit` 視角）交給遊戲自己的深度判斷器時，和真正的 Kinect v1 差多少。做法是
**同一個人、同一個動作、同一時間**：遊戲接真正的 v1（launcher 攝影機選「Kinect（Xbox 360）」），遊戲的深度檢視從 v1 深度
算出的特徵與判斷結果就是標準答案；同時另一個程式 `sfr_kinect_probe --v2` 錄 v2（兩台是不同的 USB 裝置，SDK 1.8 與 2.0
的執行環境各管各的）。兩個程式都用主機的 `steady_clock`（`steady_us`）。分開兩次錄的資料無法逐格比較（人每次動作不同），
所以不做那種比較。

### 檔案

| 檔案 | 誰寫 | 內容 |
| --- | --- | --- |
| `<前綴>-skeleton.csv` | 遊戲（v1）、探針（v1／v2） | 骨架，見上一節 |
| `<前綴>-race.csv` | 遊戲 | 比賽中每次更新一列：舊的 14 欄＋深度檢視的 19 欄（見下方） |
| `<前綴>-nui.csv` | 遊戲 | 每次輸入更新、每位玩家一列：游標、平滑後的關節、全部 32 個比賽判斷器、賽前量測（見上一節） |
| `<前綴>-depth.bin` | 遊戲 | 遊戲經 `NuiImageStreamGetNextFrame`（`82767148`）**實際拿到**的每張深度影像（v1 原樣，或 v2 合成） |
| `<前綴>-v1-depth.bin`、`<前綴>-v2-depth.bin` | 探針 `--record <前綴> --depth` | 各感測器交給遊戲時會是的影像：v1 原始深度、v2 以 `fit` 合成（`game_image()`）；一台一個檔，`--v1`／`--v2`／`--both` 都一樣 |
| `<前綴>-v2raw.bin` | 探針 `--record <前綴> --raw`，或遊戲用 v2 時 `SFR_KINECT_RECORD_RAW=1` | v2 合成每一張影像時**用到的原始資料**（格式 `SFRKRAW1`，見下方「原始 v2 錄製」），可離線用任何視角重做 |
| `<前綴>-v1-colour.bin`、`<前綴>-v2-colour.bin` | 探針 `--record <前綴> --colour` | 各感測器的**彩色畫面**（格式 `SFRKCOL1`，見下方「彩色錄製」），跟深度、骨架用同一個時鐘 |

### 彩色錄製

給 Mac 的 Kinect 用（#305）：Mac 上沒有微軟的 Kinect runtime，驅動（libfreenect、libfreenect2）只給彩色與深度，
沒有骨架和玩家編號，要自己從這兩樣做出來。同時錄下彩色、深度與 runtime 算的骨架，就能離線比對自己做的結果。

開關：`SFR_KINECT_RECORD=<前綴>` 加 `SFR_KINECT_RECORD_COLOUR=1`（探針的 `--colour` 等於後者）。v1 是 640x480，
v2 是預覽用的 v1 大小（[`kinect_v2_image`](../src/kinect_v2_image.h)，畫面外補黑邊）。每張存成藍、綠、紅三個位元組
（約 900 KB），所以預設只留每第 2 張（`SFR_KINECT_RECORD_COLOUR_EVERY`，每秒 15 張），每個檔最多
`SFR_KINECT_RECORD_COLOUR_MB` MB（預設 4096，約 4.5 分鐘；滿了停止並 log 一次 `NATIVE_KINECT_RECORD colour full …`）。
格式見 [`kinect_record_colour.h`](../src/kinect_record_colour.h)；`steady_us` 跟深度檔、骨架 CSV 是同一個時鐘，用來對齊。
v2 的彩色畫面從 #305 起才帶時間（`IColorFrame::get_RelativeTime`）。

錄一段比對用的資料（兩台都接上時用 `--both`，一台時用 `--v1` 或 `--v2`）：

```powershell
outuild\host\sfr_kinect_probe.exe 180 --both --record D:ec\mac1 --depth --colour
```

180 秒內依序：站著不動 10 秒、舉右手在畫面上移動（選單游標）、左右傾身、蹲下、跳、側移、走出畫面再走回來。

開關：`SFR_KINECT_RECORD=<前綴>` 加 `SFR_KINECT_RECORD_DEPTH=1`（探針的 `--depth` 等於後者）。每個深度檔最多
`SFR_KINECT_RECORD_DEPTH_MB` MB（預設 4096，滿了停止並在 log 寫一次 `NATIVE_KINECT_RECORD depth full <檔名> after … bytes (… images): stopped`）；
`SFR_KINECT_RECORD_DEPTH_EVERY=N` 只留每第 N 張（預設 1）。第一張影像到時才開檔，log 寫
`NATIVE_KINECT_RECORD writing <檔名> (depth 320x240, at most 4096 MB, every 1st image, … MB free)`；該磁碟可用空間少於上限的兩倍時多一行
`NATIVE_KINECT_RECORD depth low disk: … MB free for …, less than twice the cap (… MB)`。CSV 仍受 `SFR_KINECT_RECORD_MB` 限制。

### 深度檔格式 `SFRKDEP2`（[`kinect_record_depth.h`](../src/kinect_record_depth.h)，全部小端序）

檔頭 32 bytes：

| 位移 | 型別 | 內容 |
| --- | --- | --- |
| 0 | char[8] | `SFRKDEP2` |
| 8 | uint32 | 版本 2 |
| 12 | uint32 | 檔頭大小 32 |
| 16 / 20 | uint32 | 寬 320、高 240 |
| 24 | uint32 | 像素格式 1：uint16，`毫米 << 3 \| 玩家編號`（0 無、1–6），即遊戲收到的值 |
| 28 | uint32 | 每張的表頭大小 68 |

每張影像：68 bytes 表頭，接著資料。

| 位移 | 型別 | 內容 |
| --- | --- | --- |
| 0 | char[4] | `KDF2` |
| 4 | uint32 | 資料 bytes 數 |
| 8 | int64 | `steady_us`：感測器後端收到（v1）或合成完成（v2）的主機時間，對齊用的時鐘 |
| 16 | int64 | `handed_us`：交給遊戲（遊戲）或探針讀到（探針）的主機時間 |
| 24 | int64 | `sdk_time`：v1 的 `liTimeStamp`（毫秒），v2 深度幀的 `RelativeTime`（100 ns）；0 不明 |
| 32 | uint64 | `number`：影像編號，就是遊戲在 `NUI_IMAGE_FRAME` +8 看到的值（每個程式、每台感測器從 1 起算） |
| 40 | uint64 | `sdk_frame`：v1 的 `dwFrameNumber`；v2 沒有，為 0 |
| 48 | uint8 | 來源：1 真正的 v1、2 v2 合成 |
| 49 | uint8 | 編碼：0 原樣、1 零值遊程 |
| 50 | uint16 | 0 |
| 52 | float[3] | 虛擬 v1 的位移（v2 的 `KinectDepthView`；v1 為 0） |
| 64 | float | 深度倍數 `z_scale`（v1 為 1） |

資料：320×240 個 uint16（原樣，列由上而下），或零值遊程（非 0 的 token 是一個像素；0 之後跟一個 1–65535 的個數，表示那麼多個 0 像素），
兩者取小。檔尾被截斷的一張（遊戲在寫入中結束）讀取時略過並標示。

大小估計（原樣每張 153,600 bytes，零值遊程視背景約省 0–50%）：遊戲每 4 個遊戲畫格要一張（60 fps 時約 15 張／秒）約 2.2 MB/s、
每分鐘約 130 MB；探針 v2 每秒 30 張約 4.4 MB/s、每分鐘約 265 MB。預設上限 4096 MB 約可錄遊戲 30 分鐘、探針 v2 15 分鐘。

### 原始 v2 錄製 `SFRKRAW1`（[`kinect_record_raw.h`](../src/kinect_record_raw.h)，全部小端序）

目的：同一次錄影可以離線換視角、換參數重做 v2 交給遊戲的深度（`sfr_kinect_depth_resynth`），不必再錄一次。
寫入的是 v2 感應器執行緒**合成那一張影像時**用的東西：依 `RelativeTime` 配成一對的深度幀與身體索引幀、視角當時保存的最新骨架幀
（身體與地板，`KinectDepthViewer::bodies`）與兩個時鐘，所以和合成看到的是同一批幀。開關：`SFR_KINECT_RECORD=<前綴>` 加
`SFR_KINECT_RECORD_RAW=1`（探針的 `--raw` 等於後者；不加 `--raw` 時探針的輸出和以前一樣）。`SFR_KINECT_RECORD_RAW_MB` 上限（預設 16384，
滿了停止並 log 一次 `NATIVE_KINECT_RECORD raw full …`），`SFR_KINECT_RECORD_RAW_EVERY=N` 只留每第 N 張（預設 1；`ideal` 與 `player` 的狀態依收到的幀前進，
只有每張都留才能完全重現）。檔案在感應器的 mapper 第一次給出點（校正已到、表格也就有了）時才開，或約 5 秒都沒有時改用鏡頭參數；log
`NATIVE_KINECT_RECORD writing <前綴>-v2raw.bin (v2 raw depth 512x424, rays sensor-table, at most 16384 MB, every frame, … MB free)`，
可用空間少於上限兩倍時另有 `NATIVE_KINECT_RECORD raw low disk: …`；每 5 秒的 `NATIVE_KINECT model=v2` 行多 `raw_frames=`。

檔頭（64 + 8·寬·高 bytes）：

| 位移 | 型別 | 內容 |
| --- | --- | --- |
| 0 | char[8] | `SFRKRAW1` |
| 8 | uint32 | 版本 1 |
| 12 | uint32 | 檔頭大小 64 + 8·512·424 = 1,736,768 |
| 16 / 20 | uint32 | 深度寬 512、高 424 |
| 24 | uint32 | 射線來源：1 感應器自己的表（`ICoordinateMapper::GetDepthFrameToCameraSpaceTable`，第 18 格）、2 感應器的鏡頭（`GetDepthCameraIntrinsics`，不含畸變）、3 典型 v2 鏡頭 |
| 28 | float[7] | 感應器的深度鏡頭：焦距 x、y，中心 x、y（像素），徑向 2、4、6（不知道時 0） |
| 56 | uint32 | 每張的表頭大小 208 |
| 60 | uint32 | 0 |
| 64 | float[2·寬·高] | 每個深度像素一條射線（x/z、y/z，攝影機座標 +y 朝上），列由上而下 |

每張：208 bytes 表頭，接著資料。

| 位移 | 型別 | 內容 |
| --- | --- | --- |
| 0 | char[4] | `KRW1` |
| 4 | uint32 | 資料 bytes 數 |
| 8 | int64 | `steady_us`：合成完成的主機時間（與深度檔相同的時鐘） |
| 16 | int64 | 深度幀的 `RelativeTime`（100 ns；身體索引幀與它相差不到半格） |
| 24 | int64 | 最新骨架幀的 `RelativeTime`（100 ns；還沒有時 0） |
| 32 | uint64 | 合成影像的編號（與深度檔的 `number` 相同） |
| 40 | uint32 | 旗標：bit 0 = 即時合成用的是感應器 mapper 的點（否則是典型鏡頭，`kinect_v2_depth_points_from_lens`） |
| 44 | uint32 | 身體數（0–6） |
| 48 | float[4] | 該骨架幀的地板平面（全 0：沒有） |
| 64 | 6 × 24 bytes | 前「身體數」個有效：uint64 tracking id（遊戲拿到的 32 位元，`kinect_v2_short_tracking_id`）、uint8 body index（0–5，身體索引影像的值）、uint8 追蹤中（1）、uint8 SpineBase 關節狀態（0／1／2）、uint8 0、float[3] SpineBase（公尺） |

資料：512×424 個 uint16 深度（公釐，0 沒有），接著身體索引的遊程：uint32 遊程數，每段 uint8 值（0–5，255 沒有人）、uint8 0、uint16 個數（1–65535）。
檔尾被截斷的一張讀取時略過。

大小：每張約 434 KB 深度 + 幾 KB 遊程，約 0.44 MB；每秒 30 張約 13 MB/s、每分鐘約 0.79 GB，10 分鐘約 7.4 GiB；預設上限 16384 MB 約 22 分鐘。

**離線重做 `sfr_kinect_depth_resynth <v2raw.bin> --view fit|sensor|player|ideal [--distance m] [--height m] [--follow s] [--deadband m] [--fit-scale k] --out <depth.bin>`**
（[`kinect_v2_resynth.h`](../src/kinect_v2_resynth.h)）：寫出標準的 `SFRKDEP2` 深度檔（來源 2），`sfr_kinect_depth_features`、`sfr_kinect_depth_compare` 照常讀。
用的是**即時路徑同一套**視角狀態（`KinectDepthViewer`：`fit`／`sensor`／`player` 的鎖定、`ideal` 的跟隨）與合成（`kinect_v2_synthesize_depth`）：
每張先把它帶的骨架幀（是新的才）交給 `body_frame`，再以深度幀的時間 `depth_frame`，然後合成。點：旗標說即時用了 mapper 的幀用檔頭的射線
（射線朝向不對時翻正並提示），其他幀用典型鏡頭（與即時相同）。沒指定的參數用環境變數的預設；`--fit-scale` 沒指定時用檔頭的鏡頭焦距（即時也是用感應器的焦距）。
單元測試（`kinect_v2_depth`）以合成資料確認四種視角的「即時合成」與「錄成原始檔再離線重做」逐位元相同。
即時路徑的點來自 `MapDepthFrameToCameraSpace`，離線來自表格乘深度；兩者在實機上是否逐位元相同尚未驗證（應該只差浮點捨入）。

### 深度檢視的特徵（記憶體配置，由反組譯推得）

深度檢視 `82439530`（Kinect 管理器的工作執行緒）在 `82438268` 取到新影像後，對管理器 +7740、+7744 的兩個玩家槽位
（骨架槽位 0–5，負數為無；`82439578`–`824395D8`）各跑：

- `82439998(slot, manager+7584, buffer=view+72, width=view+48, height=view+52)`（`8243959C`）
- `82439D80(slot, record+640, manager+7584+8*slot, buffer, width, height)`（`824395CC`；`824395A0`–`824395C4` 算出
  `record = manager + 2784 + 800*slot` 的 +640 與 `(slot+948)*8 = 7584+8*slot`）

身體紀錄 `record = manager + 2784 + 800*slot`：建構函式 `82438A98` 在 `manager+3552`（record0 +768）寫骨架 +64、`manager+3568`（+784）寫 0，
`manager+4368`（record1 +784）寫 1……，所以 record +768 是骨架、+784 是槽位。比賽的判斷器讀的就是這份紀錄（+640／+644 即 `depth_right`／`depth_left`）。

玩家像素：`(pixel & 7) == slot + 1`（`vandc` 去掉 0xFFF8，`82439BA4`／`82439F80`／`8243A430`；與 1–6 的表比對，`82439BC4`／`82439FA0`／`8243A434`）。
深度是 `pixel & 0xFFF8`（毫米 << 3，不右移）。影像當成連續的 uint16 陣列，每列 width/8 個 8 像素向量（行 c 在第 c % 8 道）。

**第一段 `82439998`**（寫在管理器，每槽位 8 bytes 一組）：

| 位址 | 內容 | 依據 |
| --- | --- | --- |
| manager+7584+8·slot | 重心欄 = 欄總和 ÷ 像素數（無號除法，捨去） | `divwu` `82439CB4`、`stw` `82439CB8` |
| manager+7588+8·slot | 重心列 = 列總和 ÷ 像素數 | `divwu` `82439CDC`、`stw` `82439CE0` |
| manager+7632+8·slot | 最小欄 | `stwx` `82439D50`（各道 `vminuh` `82439BB4`） |
| manager+7636+8·slot | 最小列（第一個有玩家像素的列） | `stw r31,52` `82439D28`（`82439C08`–`82439C28`） |
| manager+7680+8·slot | 「最大欄」：各道最大欄先把第 e 道與第 e+4 道（欄 % 8）用 `vminuw`（`82439D30`）取**較小**者，再取四者最大（`vmaxuw` `82439D60`、`82439D6C`）；所以不一定是真正的最右欄（例：欄 100–139 的方塊得 135），遊戲就是這個值 | `stwx` `82439D78`（各道 `vmaxuh` `82439BBC`） |
| manager+7684+8·slot | 最大列 | `stw r8,100` `82439D64`（`82439C2C`–`82439C34`） |

沒有像素時重心是 (width/2, height/2)（`82439CE8`–`82439CF4`），最小欄＝width、最小列＝height、最大欄列＝0。
像素數本身只在暫存器，不寫入記憶體。各道的欄、列、個數是 16 位元飽和加法（`vadduhs` `82439BDC`–`82439BE4`），每列結束時併入 32 位元字（`82439BFC`–`82439C20`）；320×240 不會飽和。

**第二段 `82439D80`**（寫在身體紀錄 +640–+679），用第一段的重心 (cc, cr)：列 0..cr−1 是上半（`82439F68`–`82439FF0`），
列 cr..height−1 是下半（`8243A010`–`8243A098`）；欄 < cc 算左（`vcmpgtuh` cc > 欄，`82439F90`／`8243A038`），其餘算右。

| 位址 | 內容 | 依據 |
| --- | --- | --- |
| record+640 | 下左像素數（float）＝畫面左下＝玩家右側；`depth_right` | `stvx v63` `8243A2D4` |
| record+644 | 下右像素數；`depth_left` | 同上 |
| record+648 / +652 | 上左 / 上右像素數 | 同上 |
| record+656..+668 | 四區（同順序）的平均深度（毫米<<3，float）＝總和 × 1/個數（`vrefp` 加兩次牛頓法）；空的區為 65528.0（`821AC140` 的常數） | `8243A300`–`8243A3A0`、`stvx v12` `8243A3A4` |
| record+672 | near：玩家像素中深度距「平均深度取整再 & 0xFFF8」小於 600（75 mm）的個數；以有號 16 位元比較（`vcmpgtsh`） | 迴圈 `8243A42C`–`8243A468`（`8243A454`）、`stw` `8243A4A0` |
| record+676 | 平均深度 = (((上右 + 上左) + 下右) + 下左) × 0.25（`8218FCE4` 的 0.25） | `8243A3A8`–`8243A3C4`、`stfs` `8243A3C8` |

門檻：總數 `((上右 + 上左) + 下右) + 下左`（float）小於 `(width·height) >> 9`（320×240 為 150；`mullw` `8243A1D8`、`rlwinm` `8243A1E4`、`fcmpu` `8243A294`）時，
只把 +640–+655 清成 0（`stvx v0` `8243A29C`）就返回：**+656–+679 保留上一張影像的值**。

`<前綴>-race.csv` 新增的 19 欄（舊的 14 欄不變，舊檔照樣讀得進來）：`view_frame`（深度檢視這次算的影像編號）、`view_us`、`slot`、
`centroid_col`、`centroid_row`、`min_col`、`min_row`、`max_col`、`max_row`、`count_lower_left`、`count_lower_right`、`count_upper_left`、
`count_upper_right`、`mean_lower_left`…`mean_upper_right`、`near_count`、`mean_depth`。這些值在 `82439530` 的 hook 裡、深度檢視剛跑完
**同一張影像**時，在同一個執行緒從記憶體讀出（[`nui_hooks.cpp`](../src/nui_hooks.cpp)），再由比賽每次更新的那一列帶出
（[`kinect_race_record.h`](../src/kinect_race_record.h)）；影像很少時 count 為 0、其餘是舊影像的值，這和遊戲記憶體一致。
`view_frame` 與 `<前綴>-depth.bin` 的 `number` 是同一個計數器，所以可以逐張對應。

### 工具

- **`sfr_kinect_depth_features <depth.bin> [--csv out.csv] [--slot 0..5] [--summary] [--validate <前綴>-race.csv] [--show N]`**
  （[`kinect_depth_features.h`](../src/kinect_depth_features.h)）：照上面的演算法離線重算每張影像、每個有像素的槽位的特徵，
  `--csv` 每列：影像的時鐘與編號、無效像素比例、玩家像素數、槽位、像素數、重心、範圍、四區個數、是否寫入、四區平均、near、平均深度；
  最後一定印出摘要（張數、時間長度、每秒張數、平均無效像素比例、平均玩家像素、有玩家的比例）。
  `--validate` 拿遊戲的 race CSV：每個（影像編號, 槽位）只比一次（比賽每次更新都寫一列，同一張影像會重複），
  以**影像編號**對應（兩個檔用同一個計數器，不靠時鐘），逐欄完全比對（float 逐位元），印出每欄與全部欄位的完全相符率和前 N 個差異；
  影像太少時遊戲沒寫的平均、near、平均深度不比。全部相符時結束碼 0，否則 3。
- **`sfr_kinect_depth_compare <v1 depth.bin> <v2 depth.bin> [--skeleton <v1 skeleton.csv> <v2 skeleton.csv>] [--csv aligned.csv] [--report report.txt] [--max-lag-ms 1000] [--gap-ms 100]`**
  （[`kinect_depth_compare.h`](../src/kinect_depth_compare.h)）：每張影像取像素最多的槽位當玩家（v1 的骨架槽位和 v2 的身體索引不必相同）。
  先用主機時鐘對齊，再以玩家重心列（`centroid_row`）每 10 ms 重新取樣做互相關，在 ±`--max-lag-ms` 內找出 v2 比 v1 晚多少（拋物線內插到小於一步），
  印出時間差與 r；`--skeleton` 另以兩個骨架檔的 SpineBase 高度做同樣的互相關當對照。之後在每張 v1 影像的時刻（加上時間差）
  線性內插 v2（兩側影像相距不超過 `--gap-ms`、都有玩家），每個特徵印出 n、Pearson r、最小平方法 `v1 = slope·v2 + intercept`、
  RMSE（v1 − v2）、擬合後殘差 RMSE、該特徵自己的最佳時間差、兩邊平均；另印兩台的有效像素比例、平均玩家像素、失去玩家的比例（v1、v2、任一台）。
  特徵：重心、範圍、玩家像素數、四區個數、`lean`（遊戲由 +640/+644 算的傾斜，最多 ±3.5）、四區平均（空區不算）、near、平均深度、無效像素比例。
  報告用繁體中文標籤，`--report` 另存 UTF-8 文字檔，`--csv` 存對齊後的序列。
  另印輪廓大小比（`player_pixels` v2/v1）與平均深度比（`mean_depth` v2/v1），都是兩邊都有玩家的配對影像的平均。
  `--events <v1game-race.csv>`：遊戲自己判定的每一次蹲下與跳（race CSV 的 `crouch_original`／`jump_original` 每段連續 1 的第一列，依身體紀錄分開，沒問的列略過），
  在對齊後的序列上算 v1 與 v2 的 `centroid_row` 與 `min_row` 幅度：基準 = 事件前 2.0–0.5 秒的平均，反應 = 之後 1.2 秒內的最大（蹲）或最小（跳）減基準；
  印出兩邊的平均幅度、比值 v2/v1 與同方向的次數（`kinect_event_amplitudes`，有單元測試）。用 run2 驗證：12 次蹲（11 次兩邊都有資料）、11 次跳，
  蹲 `centroid_row` 34.0 對 25.5（0.751）、`min_row` 58.7 對 52.0（0.886）；跳 `centroid_row` −38.4 對 −32.0（0.833）、`min_row` −54.4 對 −71.1（1.307），與先前手算相同。
  報告最後一行 `SUMMARY key=value …` 收集重點數字給腳本用（`lean_r`、`lean_slope`、`centroid_row_r`／`_slope`、`mean_depth_r`／`_slope`、`pixel_ratio`、`depth_ratio`、
  `crouch_centroid_row_ratio` 等）。
- **`sfr_kinect_depth_resynth`**：由原始 v2 錄製用任何視角重做深度檔，見上方「原始 v2 錄製」。
- **`sfr_kinect_record_summary`**：骨架的干擾 A/B（見上一節），不變。

驗證：`sfr_kinect_depth_view_guest_test`（只在 `-Diagnostic` 建置時有；建置時用 `tests/extract_guest_functions.cmake` 從
`out/recomp/diagnostic` 複製出 `82439998` 與 `82439D80` 的生成程式碼到建置目錄，放在自己的 4 GB 客體記憶體裡跑；兩個函式讀的四個
常數照解碼後映像的值放進去：`8217D100`／`8217D110` 的 vperm 表、`8218FCE4` 的 0.25、`821AC140` 的 65528.0×4，複製到 `83E54520`）
以 600 張各式測試影像（多位玩家、深度斜坡、雜訊、破洞、沒有深度的玩家像素、貼邊、全畫面最深值的飽和情況）×6 個槽位比對離線演算法：
3600 組全部欄位逐位元相同（677 組寫入完整特徵、2923 組像素太少只清零）。`sfr_kinect_depth_test` 有手算的合成影像、檔案格式、
互相關與擬合的單元測試。還沒有用真實錄影做過 `--validate`（需要實機）。

## 觀察用 trace（`SFR_NUI_ANALYSIS_TRACE`）

為了驗證遊戲怎麼用骨架的研究（見「遊戲怎麼用骨架」）裡還沒在執行中確認的推論，`SFR_NUI_ANALYSIS_TRACE=1` 讓遊戲在
`game.log` 多寫幾種觀察行（[`nui_analysis_trace.h`](../src/nui_analysis_trace.h)）。它們**不改變**遊戲的任何行為：每個 hook 照常跑原本的函式，
之後只讀客體記憶體。每一種都有自己的上限（平均分散在每一秒裡、總行數也有上限，到了上限寫一行 `NUI_TRACE_<種類> stopped after N lines (cap)`
就不再寫），所以玩很久 `game.log` 也不會變得很大。所有行都以 `NUI_TRACE_<種類>` 開頭：

| 種類 | 在哪裡 | 每秒最多 | 總數上限 | 內容 |
| --- | --- | --- | --- | --- |
| `SMOOTH` | `NuiTransformSmooth`（`827701A0`，[`nui_smooth_hooks.cpp`](../src/nui_smooth_hooks.cpp)） | 1 | 4000 | `mode=`（`native` 原生平滑、`emulated` 模擬骨架不平滑、`host` 後端已平滑、`original` 跑原版）、`result=`（回傳值）、`initialized=`（`[82B5D2AC]`，函式庫的「已初始化」旗標）、`params=` 與五個參數、`frame_number=`、`tracked=`（框架裡 TRACKED 的骨架數） |
| `CURSOR` | 輸入更新 `824920F8` 之後（[`nui_hooks.cpp`](../src/nui_hooks.cpp)） | 10 | 36000 | 每位有身體的玩家一行：`left[...]`／`right[...]` 是該手的 `flags=`（P+0：0x8 舉起、0x10 放下）、`theta=`（P+60）、`r=`（P+148）、`target=`（P+96/+100）、`cursor=`（P+80/+84，選單讀的像素）與 `formula=`（由 θ、r 照研究的公式算的 x、y）；之後是輸入物件裡遊戲看到的關節 `hand_l= hand_r= shoulder_l= shoulder_r= head= spine=`（玩家區塊 +1264，平滑與鏡像 (−x, y, −z) 之後） |
| `TURN` | `822C6200`、`822C8958` 之後（[`nui_race_hooks.cpp`](../src/nui_race_hooks.cpp)），只在比賽中 | 5＋5 | 各 18000 | `stage=lean`：深度傾斜的 10 筆歷史（`depth_lean=` 新的在前，`depth_lean_mean=` 是 `822C63E8` 的加權平均，source +44）與身體傾角的 10 筆歷史（`tilt=`、`tilt_mean=`，source +88），以及 `depth_right=`／`depth_left=`（rec +640/+644）。`stage=steer`：讀到的 `lean_in=`（source +44）、自己的濾波 `filter_mean=`（detector +112）、寫出的 `steer=`（結果 +76，有號位元組）、`steer78=`、方向位元 `bits=`、`result=` |
| `CALIB` | `822C65B0`（雙臂往前）、`822C6AD8`（量身體）之後（[`nui_analysis_hooks.cpp`](../src/nui_analysis_hooks.cpp)） | 20 | 2000 | 每次回傳由「不是 1」變成 1 時一行：`step=arms_forward|body`、`calib=`（`[source+92]`）、`record=`、`tracking_id=`、`slot=`，以及 `+0=` 到 `+132=` 全部 34 個值（+112、+116、+120 是整數像素，其他是浮點） |
| `BIND` | 綁定 `82222B80` 之後（[`nui_analysis_hooks.cpp`](../src/nui_analysis_hooks.cpp)） | 10 | 2000 | 模式（`mode=`，box +176）、`team_mode=`（`83E515E7`，3 是接力）、兩位玩家的 rec（box +120/+124）與它們的 tracking id、enrollment（box +136/+140）、候選數（+180/+184）；其中任一個（候選數除外）改變時一行 |

**怎麼開**：遊戲從 launcher 繼承環境變數，所以在 PowerShell 裡先設再開 launcher：

```powershell
$env:SFR_NUI_ANALYSIS_TRACE = '1'
Start-Process .\FreeRidersRecompiled.exe -WorkingDirectory .
Remove-Item env:SFR_NUI_ANALYSIS_TRACE
```

**拿到有用資料要做什麼**（v1 與 v2 各跑一次，比較兩份 `game.log`）：

1. 進到主選單後，舉起右手讓游標出現；站著不動，手臂伸直，**手繞著肩膀左右慢慢掃過整個畫面**（約 90°），再上下移動。
   `NUI_TRACE_CURSOR` 的 `cursor=` 應該慢慢追上 `formula=`；水平位置只跟 θ 有關，所以同一個掃法在 v1、v2 上的 `theta=` 範圍應該一樣。
2. 看一行 `NUI_TRACE_SMOOTH`：預設是 `mode=native result=0x0`，參數應該是遊戲自己的 0.5／0.5／0.5／0.05／0.04；
   加 `SFR_NUI_SMOOTH=0` 時跑原版，預期 `mode=original result=0x8000ffff initialized=0`（函式庫沒初始化，什麼都不做）。
   兩種各玩一段，比較 `NUI_TRACE_CURSOR` 的游標抖動，就是平滑的效果（見「骨架平滑」）。
3. 開一場 Free Race（單人），在「On your Gear!」照畫面做完量測：每一步完成時應該各有一行 `NUI_TRACE_CALIB`。
4. 比賽中左右傾身、站直、再傾向另一邊：`NUI_TRACE_TURN stage=lean` 與 `stage=steer` 顯示深度傾斜與身體傾角兩條歷史怎麼合成最後的轉向。
5. 有兩個人時再開 2P、Tag 與 Relay（接力換人一次）：`NUI_TRACE_BIND` 記下每次綁定改變。

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
v2 預覽影像用到的介面（彩色、深度、身體索引的 frame source／reader／frame）
同樣是照 `Kinect.h` 的順序手寫，尚未在實機上驗證；`sfr_kinect_probe 5 --images` 存下的兩張圖是最快的檢查方式。
所有手寫的 v2 介面（`IKinectSensor`、各 frame source／reader／frame、`IBody`、`ICoordinateMapper`）
的方法槽位已逐一對照 SDK 2.0（v2.0_1409）`Kinect.h` 的 C 風格 `*Vtbl` 宣告，槽位編號寫在
[`kinect_v2_win32.cpp`](../src/kinect_v2_win32.cpp) 的註解裡；建置仍不需要 SDK（`Kinect20.dll` 執行時才載入）。
特別需要確認的：

- 選單的手部游標位置是否對得上（模擬玩家的右手在 x=0.175 為畫面中央）。
- 比賽中遊戲自己的判斷器在真實骨架上的反應，以及「On your Gear!」量測能否完成。

## 深度串流

主機上遊戲除了骨架，還讀 Kinect 的深度影像加玩家遮罩（每像素 16 位元：高 13 位元深度、
低 3 位元玩家編號，320×240，與 SDK 1.8 的 `NUI_IMAGE_TYPE_DEPTH_AND_PLAYER_INDEX` 相同，
只差位元組順序）。深度檢視物件（Kinect 管理器 +7736，建構在 `82437F38`）開啟串流
`NuiImageStreamOpen`（`82768C40`，類型 0、解析度 1），建立 4 張材質與 +72 的緩衝區；
深度檢視（`82439530`）再對每位玩家跑 `82439998`、`82439D80`，算出輪廓資料，比賽的傾斜
（+640／+644）等由此而來。函式庫沒有初始化時開啟會失敗，建構函式就丟掉物件，所以之前
一直沒有深度資料（`NUI_DEPTH_VIEW_SKIPPED`）。

遊戲也用同一個類別（函式表 `0x821A8768`）開彩色串流（類型 1、解析度 2，640×480、每像素 4 bytes）。
`SFR_CAMERA=kinect` 時接上兩條串流：

- 感應器端（[`kinect_sensor_win32.cpp`](../src/kinect_sensor_win32.cpp)）：`NuiInitialize` 多加深度與彩色
  旗標，開 SDK 的深度加玩家編號（320×240）與彩色（640×480）串流，讀骨架的迴圈順便取兩者最新一幀
  （`NATIVE_KINECT_IMAGE_OPEN type= result=`）。
- 遊戲端（[`nui_hooks.cpp`](../src/nui_hooks.cpp)）：模擬 `NuiImageStreamOpen`（`82768C40`）、
  `NuiImageStreamGetNextFrame`（`82767148`）、`NuiImageStreamReleaseFrame`（`82767458`）。開啟時照物件
  自己的格式（深度 `0x28280044`、彩色 `0x28280086`）用遊戲的 `824F3EA0` 建一張材質；取幀時把感應器
  的影像以大端序寫進這張材質（深度 16 位元原樣、彩色 X8R8G8B8），填好 `NUI_IMAGE_FRAME`（+20 材質）
  交給遊戲。之後的上色（`82438328`）、+72 緩衝區、輪廓計算全是遊戲自己的程式。
  `NUI_IMAGE_STREAM_OPEN ... backend=kinect texture=`、前 4 幀的 `NUI_IMAGE_FRAME type= number=` 可確認。

有了深度影像，遊戲自己的深度檢視會算出傾斜那一對數值；`SFR_KINECT_RECORD` 的 `<前綴>-race.csv`
以 `depth_right`／`depth_left` 記下它。

### 全部交給遊戲

選 Kinect v1 時一定會開啟深度與彩色串流，選 Kinect v2 時一定會合成深度串流（見下一節）。傾斜由遊戲的深度檢視計算，
蹲下與起跳沿用遊戲判斷器，也不做水平校正。**沒有骨架備援**：v1 的深度串流打不開、或 v2 的深度與身體索引讀取器打不開時，
遊戲的 `NuiImageStreamOpen` 照常失敗（交給原本的函式，在沒有初始化的函式庫上就是失敗，與沒有深度的主機一樣），
log 寫 `NATIVE_KINECT_DEPTH unavailable model=Kinect v1|Kinect v2 reason=…`（`reason`：`no-image-api`、`stream-0x…`、
`depth-0x…`、`body-index-0x…`），launcher 的 Kinect 預覽也會寫「遊戲沒有深度影像」與原因。

### Kinect v2 深度

（沒有骨架備援：讀取器打不開時遊戲沒有深度，只記 log 並在 launcher 預覽顯示；影像中斷超過 0.5 秒只在恢復時記一行 `NUI_KINECT_DEPTH gap ms=…`。）

主機端用骨架猜的傾斜與蹲跳是 v2 玩家轉向、跳躍不準的原因（已移除）：主機上傾斜的那一對數值與蹲、跳的深度部分來自遊戲自己的深度檢視
（蹲、跳另外也讀骨架的髖中心；其他判斷器讀骨架，見「遊戲怎麼用骨架」）。
Kinect v2 一律（沒有開關）交給遊戲一條和 v1 一樣的
深度加玩家編號串流，遊戲原本的深度檢視與判斷器照 v1 的方式運作
（[`kinect_v2_depth.h`](../src/kinect_v2_depth.h)，有單元測試 `kinect_v2_depth`）。Kinect v1 不受影響。

- **怎麼做**：v2 的感應器執行緒另開一組深度與身體索引讀取器（與預覽的分開，互不搶畫面），兩者 `RelativeTime` 相同時，
  用感應器自己的 `ICoordinateMapper::MapDepthFrameToCameraSpace`（vtable 第 14 格，對照 SDK 2.0 `Kinect.h`）把每個
  512×424 的深度像素換成攝影機座標（拿不到時用 v2 典型鏡頭，log 的 `depth_lens_frames=`），移到虛擬 v1 相機，
  再用 v1 的標稱鏡頭投影成 320×240：`x = 160 + X·285.63/Z`、`y = 120 − Y·285.63/Z`（`NuiImageCamera.h` 的
  `NUI_CAMERA_DEPTH_NOMINAL_FOCAL_LENGTH_IN_PIXELS`，`NuiSkeleton.h` 的 `NuiTransformSkeletonToDepthImage`；像素 n 在 n）。
  同一像素取最近的點（z-buffer），玩家編號是那一點的身體索引 + 1（與骨架槽位 + 1 相同）。只填一像素寬的洞，
  而且左右或上下兩鄰居都有值、相差 5 公分內、同一位玩家；其他空的都是「沒有值」。
- **格式**：與 v1 路徑交給遊戲的相同：`毫米 << 3 | 玩家（1–6）`。v1 的串流以旗標 0 開啟（沒有
  `NUI_IMAGE_STREAM_FLAG_DISTINCT_OVERFLOW_DEPTH_VALUES`），太近、太遠、未知都回報 `NUI_IMAGE_DEPTH_NO_VALUE`（0），
  所以合成的影像也是：虛擬相機看到的距離不在 800–4000 公釐（`NUI_IMAGE_DEPTH_MINIMUM`／`_MAXIMUM`）內，或沒有點，就是 0。
- **虛擬相機放哪**（`SFR_KINECT_V2_DEPTH_VIEW`，開發者調校用，三種）：
  - `fit`（預設）：虛擬 v1 就在 v2 的位置，但把每個攝影機座標點的深度乘上 k（`(X, Y, k·Z)`）再用 v1 鏡頭投影，
    v2 的整個視野就收進 320×240（左右各留約 15 欄空白）。k = 285.63 ÷（v2 深度焦距 × 240/424）：焦距用感應器自己的
    `ICoordinateMapper::GetDepthCameraIntrinsics`（vtable 第 19 格，`FocalLengthY`；感應器送出校正前先用典型的 366，k ≈ 1.378），
    `SFR_KINECT_V2_DEPTH_FIT_SCALE`（1.0–2.0）可直接指定，1.0 就等於 `sensor`。這是 z 方向的拉長，不是把影像縮放：
    真實距離 d 的人在遊戲看來和 v1 前 k·d 處同樣大小的人一樣，深度值也是 k·d，所以畫面大小與深度一致。
    換算後不在 800–4000 公釐的仍是「沒有值」，所以實際可用的距離約是 0.58–2.90 公尺（k = 1.378）。沒有任何狀態或鎖定，
    一人、兩人、接力換人都交給遊戲自己處理。**取捨**：沿 z 的深度差也被放大 k 倍（往前、往後的動作在遊戲看來大 k 倍），
    畫面角度被壓縮成 1/k（左右移動在畫面上變小）；比 v1 能站得近，但最遠只到約 2.9 公尺。
  - `player`：換 v2 往往就是要站得比 v1 允許的近，所以虛擬 v1 不放在 v2 的位置，而是沿 v2 的 z 移動，讓玩家的參考點
    （身體中心，v2 的 SpineBase）落在 `SFR_KINECT_V2_DEPTH_DISTANCE` 公尺（預設 2.5，1–4）；知道地板時再沿 v2 的 y 移動，
    讓相機像放在地板上方 `SFR_KINECT_V2_DEPTH_HEIGHT` 公尺（預設 0.8，0.2–2）的 v1 那樣看到參考點。只平移，不轉向，也不左右移。
  - `sensor`：虛擬 v1 就在 v2 的位置（只換鏡頭與解析度，看到的是 v2 視野中間 v1 那麼寬的部分）。
  - `ideal`（launcher 的預設「Recompiled」；遊戲沒有設這個變數時仍是 `fit`）：v2 量到的是真實的 3D 場景，所以**模擬一台擺在理想位置的真正 v1**：把 v2 的點雲從一台
    **水平**、v1 鏡頭、**不拉長 z** 的虛擬相機重新投影，輪廓大小、深度值、動作幅度都是真正的 v1 在參考距離會給的
    （[`KinectIdealState`](../src/kinect_v2_depth.h)，有單元測試）。每張深度影像做一次（深度幀自己的 `RelativeTime` 當時間），
    是「前一個狀態、這一格的時間、最新骨架幀的身體與地板」的純函式，所以原始錄製（見「原始 v2 錄製」）離線重做會得到一樣的影像。
    1. **水平**：由 v2 的 `FloorClipPlane`（法向量朝上，與其他地方的重力慣例相同：v2 的重力就是地板法向量）算出旋轉
       `R = Rx(俯仰)·Rz(側傾)`，先繞相機 z 去掉法向量的 x，再繞相機 x 去掉 z，使 `R·n = (0, 1, 0)`；光軸方向不變（沒有偏航）。
       每個攝影機座標點都先轉 `R`。地板平面先平滑（時間常數 1 秒；第一個有效的直接採用，無效的那格不更新）。
       法向量與距離分開處理：v2 很早就給出法向量（重力），但還沒真正看到地板時（或一直看不到時）`FloorClipPlane` 的 W（感應器離地高度）是 0。
       以前把這個 0 當成高度，相機就被放在感應器上方 `HEIGHT` 公尺（離地約 1.7 公尺），玩家在模擬 V1 畫面裡偏下、腳被切掉；
       現在 W 至少 0.05 公尺才算知道高度（第一個直接採用、之後平滑、W 為 0 的那格不更新），不知道高度時只做水平、相機留在感應器自己的高度。
       log 會在高度出現或消失時寫一行 `NATIVE_KINECT_V2_DEPTH ideal floor height found …`／`… floor without a height (W=0) …`（每秒最多一行），
       每 5 秒的狀態行也多 `floor_given=a,b,c,d sensor_height= camera_height= height=`（感應器給的地板、感應器離地高度、相機離地高度、設定的高度）。
       一直沒有有效地板時不轉、相機在感應器自己的高度，log 一次 `NATIVE_KINECT_V2_DEPTH ideal no floor plane yet`；找到時 log 一次 `... ideal floor found: levelled`。
    2. **位置**（轉正之後只平移）：參考點是**所有**被追蹤的人 SpineBase 的平均（一人、兩人都行），在轉正後的座標；
       相機在參考點正前方 `SFR_KINECT_V2_DEPTH_DISTANCE` 公尺（預設 2.5，1–4）、左右對齊參考點、高度是地板上方
       `SFR_KINECT_V2_DEPTH_HEIGHT` 公尺（`ideal` 預設 1.0，0.2–2；`player` 仍是 0.8）。高度只跟地板，**永遠不跟玩家**。
       參考點的 x、z 各自以時間常數 `SFR_KINECT_V2_DEPTH_FOLLOW` 秒（預設 2.0，0.2–20）的指數平滑**慢慢跟**，
       而且只跟超出死區 `SFR_KINECT_V2_DEPTH_DEADBAND` 公尺（預設 0.15，0–1）的部分：死區內（傾身、小步側移、跳）相機不動，
       動作照樣留在影像裡；長時間的漂移、有人加入或離開則平順滑過去（相機最後停在離參考點一個死區寬的地方）。
       沒有人時保持最後的位置；沒人之後第一個出現的人（以及第一次找到地板時）直接跳到位，不滑。
    3. **投影**：`x = 160 + X·285.63/Z'`、`y = 120 − Y·285.63/Z'`（Z' 是到虛擬相機的距離），深度值就是到虛擬相機的公釐，
       z-buffer、一像素補洞、打包與「800–4000 公釐以外是沒有值」都和其他視角相同，玩家編號仍是 body index + 1。
       因為 D = 2.5，玩家本來就在 2.5 公尺；離虛擬相機 0.8–4 公尺的東西會保留（log 的 `true_range=0.80-4.00m from the virtual camera`）。
    4. 交給遊戲的骨架（關節、身體中心、指尖拇指；全 0 的點維持 0）、地板平面與重力做同樣的旋轉＋平移（不拉長），骨架與深度一致；
       轉正後地板是 `(0, 1, 0, H)`、重力是 `(0, 1, 0)`。預覽視窗疊骨架時 `image_point` 先把旋轉與平移還原再對到 v2 的影像。
    - **取捨**：v2 沒看到的地方（虛擬相機的視野超出 v2 視野的部分）一律空著；換了視點之後，被擋住的表面只有 v2 當初看到的部分會出現，
      其餘是洞（不補）；v2 看到的角度不變，所以近距離時輪廓的側面、底面不會像真正擺在那裡的 v1 那樣看得到。
      跟隨慢，所以大步側移的開頭仍會在影像裡移動，之後才慢慢回到中間（超出死區的部分）。
      `SFR_KINECT_RECORD_DEPTH` 的深度檔只記錄平移（`view_origin`）與 z_scale，不記錄旋轉。
  - **骨架與深度分開調**：遊戲從不把骨架投影到深度影像，兩者在幾何上不必一致，所以可以各自調整。注意骨架不只給選單：
    游標、玩家身分、轉向的身體傾角、蹲跳的髖中心、約 20 個比賽判斷器與賽前量測都讀它（深度只給傾斜那一對數值、蹲跳的輪廓與賽前量測的輪廓），
    所以骨架的轉換也會改變這些判斷（`fit` 的 z 拉長會讓含 z 的部位長度與往前伸的動作都變大，這也是預設改成 1.0 的原因之一）：
    - `SFR_KINECT_V2_SKELETON_STRETCH`（只對 `fit`，1.0–2.0；**不設＝1.0**，`depth`＝與深度相同的 k，也就是 2026-10-19 以前的預設）：
      只改交給遊戲的骨架與地板的 z 拉長，深度不變。1.0 就是感應器真實的座標（公尺），所以預設交給遊戲的骨架和真正的 v1 一樣是真實距離；
      深度照樣拉長 k 倍。遊戲從不把骨架投影到深度影像（不呼叫 `NuiTransformSkeletonToDepthImage`），兩者不必一致。
    - `SFR_KINECT_V2_SKELETON_DISTANCE`（只對 `ideal`，1–4 公尺；不設＝深度的 `SFR_KINECT_V2_DEPTH_DISTANCE`）：骨架用的虛擬相機
      沿 z 移到離參考點這麼遠（同樣水平、同樣左右與高度），深度不變。
    - 對游標的影響：游標的水平位置是「肩→手」方向和 X 軸的 **3D 夾角**（見「選單游標的公式」），不除以 Z。骨架的 z 拉長 k 倍時，
      手往前伸的 z 分量也變成 k 倍，單位向量的 x 分量變小：手臂直直往前伸時水平靈敏度最多只剩 1/k（k = 1.378 時約 0.73，要多移動約 38%）。
      預設的 1.0 讓靈敏度回到真正 v1 的大小；垂直只用 y 的比值，不受影響。`ideal` 的骨架距離只是沿 z 平移，夾角不變，所以不影響游標。
    - 兩個值都寫在開始的 `NATIVE_KINECT_V2_DEPTH on …` 行（`skeleton_stretch=1.000 (real metres)`、`skeleton_stretch=depth's (1.378)`；
      `skeleton_distance=`，不設時為 `depth's`）。`fit` 時每 5 秒的狀態行一定有 `skeleton_view=x,y,z skeleton_z_scale=`（實際交給遊戲的骨架倍數），
      `ideal` 設了骨架距離時也有。預覽視窗疊骨架時照骨架自己的轉換還原。
  - **參考點是鎖定的**，不會一直跟著玩家：第一次有人被追蹤時，取最靠中間的人，前 15 格（0.5 秒）平均後鎖定
    （這 0.5 秒先用第一格的位置），之後只要還有人被追蹤就不動；所有人離開 1 秒後才放開，下一個人走進來再重新鎖定。
    所以側移、蹲下、跳起在影像裡仍是移動，不會被抵銷。
  - 交給遊戲的骨架（關節、身體中心、指尖拇指）與地板平面做同樣的平移（`player`、`ideal`），`fit` 預設不拉長（真實公尺，見上方
    `SFR_KINECT_V2_SKELETON_STRETCH`；設成 `depth` 才和深度一樣拉長，地板法向量重新正規化，重力跟著地板法向量）。遊戲不把骨架投到深度像素，
    所以兩者不一致也沒關係；預覽視窗疊骨架時會換回 v2 自己的座標。深度模式下不做水平校正（同 v1）。
  - **取捨**：虛擬相機移到 v2 後面時，v2 的視野（約 70°×60°）在 2.5 公尺處比 v1 的 58.5°×45.6° 窄，影像邊緣可能空著，
    往旁邊大步移動會先出 v2 的畫面；被身體擋住的背景本來就量不到，平移不轉向的情況下新露出的洞很少，留空不補。
    放開鎖定前若換了站位，玩家在虛擬影像裡會偏前或偏後，離開畫面 1 秒再回來就會重新鎖定。
- **比賽**：完全由遊戲自己的判斷器決定（深度與骨架，見「遊戲怎麼用骨架」），主機端沒有替代判定，也沒有任何開關。讀取器打不開時遊戲就沒有深度
  （log 寫 `NATIVE_KINECT_DEPTH unavailable model=Kinect v2 reason=depth-0x…` 或 `body-index-0x…`，launcher 預覽也會顯示）；
  超過 0.5 秒沒有新深度影像（例如暫停或感應器停送）只在影像恢復時 log 一行 `NUI_KINECT_DEPTH gap ms=…`，行為不變。
  骨架平滑、選單游標穩定、暫停手勢不變。
- **log**：開始時 `NATIVE_KINECT_V2_DEPTH on view=fit scale=1.378 focal=366.0 (typical) true_range=0.58-2.90m depth=0x00000000 body_index=0x00000000`
  （`player` 是 `distance= height=`；`ideal` 是 `distance=2.50 height=1.00 follow=2.00 deadband=0.15 true_range=0.80-4.00m from the virtual camera (the players at 2.50m)`，
  每 5 秒的狀態行另有 `depth_camera=x,y,z level=0|1 pitch= roll= floor_d= reference=x,z reference_bodies=`（虛擬相機在轉正後座標的位置、去掉的俯仰與側傾角度、地板距離、跟隨中的參考點）；讀取器打不開時是 `off`；fit 等到感應器給出鏡頭參數時另有一行 `NATIVE_KINECT_V2_DEPTH fit scale=… focal=… (sensor) true_range=…`）、`NATIVE_KINECT started=1 model=Kinect v2 depth=1`、遊戲開啟串流時
  `NUI_IMAGE_STREAM_OPEN type=0 resolution=1 ... backend=kinect`；每 5 秒的 `NATIVE_KINECT model=v2` 行多
  `depth_synth_frames=`（約 150）、`depth_valid=`（最新一張有深度的比例）、`depth_player_pixels=`、`depth_view=`（虛擬相機的位移；fit 時為 0）
  與 `depth_latched=`；`NUI_IMAGE_FRAMES depth=給/要` 是遊戲實際拿到幾張。
- **錄製**：`SFR_KINECT_RECORD=<前綴>` 加 `SFR_KINECT_RECORD_DEPTH=1` 把**遊戲實際拿到**的每張深度影像存成 `<前綴>-depth.bin`
  （格式 `SFRKDEP2`，含虛擬相機位移與深度倍數 k；見下方「深度錄製與分析工具」），之後可以和真正 v1 的影像比較。
  舊的 `SFRKDEP1`（v2 合成的每一張，不論遊戲有沒有拿）已移除，工具讀到會說明不支援。
- **不開遊戲**：`sfr_kinect_probe 10 --v2 --game-depth [--images]` 讀同一條合成路徑（v2 一律合成，旗標只決定要不要讀取並印出），每半秒印出 `game_depth_frames= valid= player_pixels=`，
  `--images` 另存第一張為 `kinect_game_depth.bmp`。
- **兩位玩家**：遊戲的深度檢視（生成的程式碼 `82439998`、`82439D80`）以玩家槽位查一張 1–6 的表，每個像素先去掉 `0xFFF8`
  只留低 3 位元的玩家編號，只累加編號等於「槽位 + 1」的像素：`82439998` 算該玩家的重心與範圍，`82439D80` 以**該玩家自己的**
  重心欄／列把他的像素分成左右（上下）再加總深度與像素數，就是 +640／+644 的來源。沒有用影像左右兩半分人，所以兩人只要
  身體都在影像內、像素帶著各自的玩家編號（這裡就是 body index + 1，與骨架槽位一致）即可，站哪裡都行。
- **接力賽**：遊戲等上一棒走出感應器、下一棒走進來（[Relay Race](relay-race.md)；手把模擬時是 45 格沒有骨架），所以實機交棒
  中間通常有一段沒有人被追蹤的時間。`fit` 沒有狀態，不受影響；`player` 的鎖定在所有人離開 1 秒後放開，下一棒會重新鎖定；
`ideal` 在沒人時保持相機，下一棒一出現就直接跳到他前面。

**還沒驗證的**：遊戲的深度判斷器在合成影像上的反應（輪廓的大小、邊緣、相機高度是否像真正的 v1）；主機的 NUI 是否用不同的特殊值；
`MapDepthFrameToCameraSpace` 在實機上的結果；兩位玩家同時時的表現。`ideal` 只有單元測試與合成資料，沒有實機或真實錄影
（run1、run2 沒有原始檔）；v2 `FloorClipPlane` 的雜訊、`GetDepthFrameToCameraSpaceTable` 在實機上的內容與朝向（離線工具會檢查並翻正），
以及即時（mapper）與離線（表格 × 深度）的點是否逐位元相同，都還沒在實機上確認。

### 遊戲怎麼用骨架

（詳細的研究與位址在 repo 外的 `NUI_SKELETON_ANALYSIS.md`；這裡是結論。）以前這份文件說「比賽只靠深度、骨架只負責選單」，那是錯的：

- **骨架**：遊戲每格把 NUI 框架（平滑後，見「骨架平滑」）的 20 個關節乘上鏡像 diag(−1, 1, −1)（`82438F80`，不用框架的重力，所以不做水平校正）
  寫進每個槽位的身體紀錄（管理器 +2784 + 800·槽位）。讀它的有：選單游標（`82492F30`）、舉手／放下、面向感應器、玩家身分與綁定
  （`82222B80`），以及比賽中約 20 個判斷器：舉手 `822C8650`、蓄力 `822CB840`、側移 `822CA6B0`、煞車 `822C9BF0`、抓取 `822CB0B8`、
  踢地 `822CA518`／`822CBD28`、招式 `822CA810`／`822CBF30`、站姿 `822CAC90`、特技 `822C9938`、加速 `822C9A80`／`822CAF48`、飛行 `822C9180`、
  道具（上投、下揮、充氣、擦螢幕）、拉桿、前彎、把手、牽手，還有轉向的身體傾角（`822C6200` 的 head − spine、`822C8958` 的肩中心 − 髖中心）。
- **深度**：只用在四個地方：傾斜那一對數值（rec +640／+644）、蹲下（重心列，加上髖中心 z）、跳（重心列與最小列，加上髖中心 y）、
  賽前量測（重心、輪廓高度）。蹲與跳都是相對於每位玩家自己在「On your Gear!」量到的基準（`calib`）。
- 遊戲從不呼叫 `NuiTransformSkeletonToDepthImage`，骨架與深度在幾何上不必一致。
- 遊戲最多兩副 TRACKED 骨架（見 `KinectPlayerSlots`）。

`SFR_KINECT_RECORD` 的 `<前綴>-nui.csv` 把上面這些消費者每一次的結果都記下來（見「錄製」），`SFR_NUI_ANALYSIS_TRACE=1` 則在 log 裡觀察（見「觀察用 trace」）。

### 選單游標的公式

寫游標的是 `82492F30`（輸入物件 `[83E52FB8]`，每個遊戲畫格、每位玩家一次；骨架只在新框架時改變）。每隻手的封包
P = 輸入物件 + 32 + 2544·玩家 + 16 + 400·手（左手用關節 4、5、7，右手 8、9、11），坐標都是遊戲的鏡像座標：

- **水平**：θ = 「肩 − 手」單位向量和 X 軸的 **3D 夾角**（度，P+60），目標 x = 1280·(135° − θ)/90°（P+96）。θ = 135° 在左緣、90° 在中央、45° 在右緣：
  **手繞著肩膀轉 90° 就掃過整個畫面寬**，和距離、身高都無關（不除以 Z，所以以前猜的「透視投影」不對）。
- **垂直**：r = (head − hand).y ÷ (head − spine).y（P+148），目標 y = 720·(r − 0.23)（P+100）：畫面的高度等於頭到 spine 的長度，
  手和頭同高時在上緣以上（被夾住），r = 1.23 在下緣。
- **遊戲自己的游標平滑**：每個遊戲畫格 P+80 += (P+96 − P+80)·0.065（`82196FCC`；P+8 的 0x2 位元設起時直接跳到目標），
  時間常數約 15 格（60 fps 時約 0.25 秒，fps 低時更慢）；之後 `822448B0` 把 x 夾在 0..1256、y 在 0..708，交給選單的游標物件。
  這是骨架平滑（`NuiTransformSmooth`）**之外**的第二層；主機端的「選單游標穩定」（見下一節）是第三層，只在選單、只對手部。
- **出現與收起**：前臂（肘 − 手）與 +Y 的夾角 ≥ 90°（手不低於手肘）連續 20 格舉起游標（P+0 |= 8）；≤ 30° 連續 10 格放下（|= 16）。
- 和模擬器的兩次實測吻合（水平約 2600 px/m、垂直 1376 px/m，見 `nui_hooks.cpp`）。
- **對 Kinect v2 的影響**：水平用的是含 z 的 3D 夾角，`fit` 的骨架 z 若拉長 k 倍，手往前伸時單位向量的 x 分量變小，中央附近的水平靈敏度是
  `1/√(dy² + (k·dz)²)` 對 `1/√(dy² + dz²)`，手臂直直往前伸時降成 1/k ≈ 0.73。所以 `fit` 的骨架預設改成真實公尺（`SFR_KINECT_V2_SKELETON_STRETCH`
  預設 1.0）；垂直只用 y 的比值，不受影響。v2 的 Head 比 v1 高、SpineMid 和 v1 的 Spine 高度不同，r 的尺度可能還要校準（尚未實機比較）。
- 驗證：`SFR_NUI_ANALYSIS_TRACE=1` 的 `NUI_TRACE_CURSOR` 每行有 θ、r、目標、游標與照公式算的 `formula=`；`<前綴>-nui.csv` 有每一格的值。

### 骨架原樣交給遊戲

實體 Kinect 的骨架照感應器給的樣子寫進遊戲：在 SDK 自己的 6 個骨架位置、用 SDK 自己的追蹤編號
與關節狀態（[`nui_hooks.cpp`](../src/nui_hooks.cpp) 的 `write_kinect_body`）。所以：

- 深度圖的玩家編號（骨架位置 + 1）與骨架指的是同一個人，深度檢視（`82439530`）找得到玩家的
  像素，比賽的傾斜（身體紀錄 +640／+644，就是深度第二段 `82439D80` 的輸出）由遊戲自己算出。
- 走出鏡頭再回來的人是 SDK 給的新編號、新骨架，遊戲會像主機上一樣重新辨識（選單的玩家插槽
  `82491458` 狀態 1 在等骨架 +8 的辨識結果）；兩兩接力換人也是如此。
- 骨架的使用者編號（+12）是它代表的 Xbox 使用者 0–3，不是骨架位置：被辨識成登入帳號的人是 0，
  其他人依序 1、2、3。選單的玩家插槽只接受小於 4 的使用者編號，存檔也跟著這個使用者；
  填成骨架位置（可能是 4、5）時遊戲會改走登入畫面，不讀存檔。
- 辨識結果依 SDK 追蹤編號記錄（`NuiIdentityIdentify`）：目前沒有其他被追蹤的人占著登入的設定檔
  時就是設定檔，否則是訪客；編號不再被追蹤就忘掉。log：`NATIVE_KINECT_ENTER tracking_id= slot=`。

原本的做法（把被追蹤的人重新排到第 1、2 格、編號固定 1、2）只留給手把與 webcam 的模擬玩家。

### 仰角與平滑

- 感應器角度在預覽視窗裡調整（見下方「預覽視窗」）：「往上」「往下」或鍵盤 ↑↓，每次用 Kinect 的馬達
  轉 5 度（SDK 的 `NuiCameraElevationSetAngle`，−27～27 度；SDK 要求馬達一秒最多轉一次，轉動時按鈕停用）。
  感應器斷電前會維持角度。
- 骨架平滑：主機的執行環境交給遊戲的是未平滑的骨架，**遊戲自己呼叫 `NuiTransformSmooth`**，見下一節。

### 骨架平滑（遊戲自己的 `NuiTransformSmooth`）

遊戲每拿到一張新骨架框架，就在 Kinect 管理器的更新（`82438D80`，呼叫處 `82438E58`）對自己那份框架（管理器 +16）
呼叫 `NuiTransformSmooth`（`827701A0`），參數在管理器 +2752，是建構函式（`82438A98`）寫入的 SDK 預設值
0.5／0.5／0.5／0.05／0.04，之後才把關節轉成身體紀錄。所以選單游標、比賽中約 20 個讀關節的判斷器與賽前量測，原本拿到的都是平滑過的骨架。

**之前的問題**：`827701A0` 一開頭就檢查函式庫的「已初始化」旗標 `[82B5D2AC]`，這個旗標只有原版 `NuiInitialize`（`8276FD10`）會設，
而我們的 `NuiInitialize` hook 不跑原版，所以它一直回傳 `0x8000FFFF`、什麼都不做，遊戲也不看回傳值：v1 的骨架一直沒有被平滑；
v2 則是主機端自己的平滑碰巧補上。`SFR_NUI_ANALYSIS_TRACE=1` 的 `NUI_TRACE_SMOOTH`（加 `SFR_NUI_SMOOTH=0`）可以看到 `result=0x8000ffff initialized=0`。

**現在**（[`nui_smooth_hooks.cpp`](../src/nui_smooth_hooks.cpp)、[`nui_transform_smooth.h`](../src/nui_transform_smooth.h)，有單元測試 `nui_transform_smooth`）：
hook `827701A0`，以原生方式做函式庫的濾波，用遊戲傳進來的參數（r4 指向五個大端序 float；r4 為 0 時用函式庫預設），直接改遊戲的大端序框架，回傳 `S_OK`。
演算法照遊戲連結的函式庫（`827701A0` 對每個槽位呼叫 `82779700`）逐步讀出：

- 6 個骨架槽位各有 20 個關節的歷史（原始值、濾波值、趨勢、已看過幾格）。槽位不是 TRACKED（2）就清掉歷史、不濾波；
  我們另外在槽位的 tracking id 改變時清掉（換了一個人；主機的函式庫沒有檢查這個）。
- 關節位置剛好是 (0, 0, 0) 時重新開始，原樣交出；因為它成了新歷史的第一格，下一格是它和原點的平均（再被最大偏差拉回量測值 4 公分內），這是函式庫（與 SDK 範例）原本的行為。
- 關節的追蹤狀態只影響半徑：推測（1）的抖動半徑與最大偏差加倍；「沒追蹤」（0）但有位置的關節照樣濾波。
- 抖動半徑最小 0.0001 公尺；骨架的位置（中心，+16）不濾波；每個關節的 w 改成 1。
- 第一格：量測值、趨勢 0。第二格：兩次量測的平均，趨勢往差值修正。之後：離上一個濾波值不到抖動半徑時，依移動距離比例拉向上一個輸出；
  濾波值 =（1 − smoothing）·量測 + smoothing·（上一個濾波值 + 上一個趨勢）；趨勢 = correction·（濾波值 − 上一個濾波值）+（1 − correction）·上一個趨勢；
  輸出 = 濾波值 + prediction·趨勢，離量測值超過最大偏差時拉回到最大偏差上。
- 骨架 hook 在感測器沒有新框架時會把上一張再交給遊戲（框架編號 +8 相同）；同一個框架編號不會平滑第二次，而是再寫一次上一次的輸出。

**只平滑實體 Kinect（v1、v2）的骨架**。手把模擬與 webcam 體感的骨架原樣保留，但一樣回傳 `S_OK`：主機上執行環境追蹤到的骨架全部會被平滑，
但模擬的骨架不是感測器量的——手把的骨架是精確的、只隨手把移動，webcam 的骨架已經過 `pose_smoothing`——再平滑只會讓每個選單與比賽動作多一格左右的延遲。

**一副骨架只平滑一次**：v2 主機端的平滑（`KinectJointSmoother`）與 v1 的 SDK 平滑預設都關閉；`SFR_KINECT_SMOOTH=1` 時改由後端平滑（給
`sfr_kinect_probe --smooth` 與測試用），遊戲這邊就不再平滑。`KinectJointSmoother` 現在用的就是同一個濾波步驟（單元測試確認兩者逐位元相同）。
錄製（`<前綴>-skeleton.csv`）一律記錄平滑**之前**的骨架。`SFR_NUI_SMOOTH=0` 改跑原版（什麼都不做，和以前一樣），方便比較。

log：第一次與模式改變時一行 `NUI_TRANSFORM_SMOOTH mode=native|emulated|host|original result=0x…`；`SFR_NUI_ANALYSIS_TRACE=1` 時每秒最多一行
`NUI_TRACE_SMOOTH mode=… result=… initialized=… smoothing=0.5 correction=0.5 prediction=0.5 jitter_radius=0.05 max_deviation_radius=0.04 frame_number=… tracked=…`。

**和真正的 SDK 比對**：`sfr_nui_transform_smooth_sdk_test`（Windows，CTest `nui_transform_smooth_sdk`）載入 SDK 1.8 的 `Kinect10.dll`，
把同一串合成的骨架框架（兩個人、晃動加雜訊、推測與沒追蹤的關節、遺失成原點的關節、離開再回來；預設參數與另外五組參數）
同時交給 SDK 的 `NuiTransformSmooth` 與我們的實作，比對每個關節（容許 0.1 公釐）。**它不呼叫 `NuiInitialize`、不啟動任何感測器**。
在這台開發機上，沒有初始化時 SDK 的 `NuiTransformSmooth` 對空框架與有骨架的框架都回傳 `0x8007000E`（E_OUTOFMEMORY，和主機函式庫
配置歷史緩衝區失敗時回的一樣），不做任何事，所以這個測試回報 `SKIPPED` 並以 77 結束（CTest 記為略過）；演算法的正確性改由
`nui_transform_smooth_test` 依函式庫的程式碼逐步手算確認。在已初始化感測器的程式裡比對（例如 `sfr_kinect_probe`）還沒有做。

### 選單游標

骨架照原樣交給遊戲（遊戲自己再平滑骨架、再平滑游標，見「骨架平滑」與「選單游標的公式」），只有選單游標和主機不同（[`kinect_stability.h`](../src/kinect_stability.h)，和 webcam 共用
[`pose_stability.h`](../src/pose_stability.h) 的 `MenuHandSteadying`；尚未實機確認效果）：選單中（不在比賽時）雙手以肩膀為準，
靜止時小幅晃動不移動游標，慢慢移動立刻跟上，快速移動完全不延遲，單一格的突跳忽略。比賽中不作用。每個被追蹤的人各自記憶，
依 SDK 追蹤編號區分。靜止圈半徑在啟動器「體感」頁的「選單游標穩定」設定（1–30 公釐，預設 8，0 關閉；`SFR_MENU_STEADY_RADIUS`、`SFR_MENU_HAND_STEADY=0`）。遊戲視窗的標題列每半秒顯示 `[FPS 59.8 | 16.7/41.0 ms]  [Kinect 30]  [Vulkan 100%]`：FPS、平均／最長畫格時間、遊戲每秒讀到幾格新骨架、繪圖後端與渲染倍率；啟動器「畫面與效能」頁的「標題顯示效能數字」可以關掉（`SFR_TITLE_STATS=0`）。

用實體 Kinect 比較：`sfr_kinect_compare`（Windows，先關遊戲，Kinect 一次只給一個程式）開一個四格視窗，上排是 SDK 原樣、
下排是游標穩定處理後，左邊彩色、右邊深度。M 切換比賽／選單（預設選單，畫出右手的游標軌跡），1 開關下排的游標穩定。

**試過、拿掉的**：被遮住的四肢點固定（遮擋保持，含骨長判斷）與用深度影像檢查推測位置（深度分層）曾經套用到 Kinect，
用四格比較視窗在實機上看不出差別，所以拿掉了；SDK 1.8 在一般遊玩姿勢下推測關節的情況不多。webcam 的遮擋保持仍保留
（[攝影機體感](camera-input.md)）。

### 預覽視窗

啟動器 Kinect 設定的「Kinect 感測器」→「開啟預覽」開一個視窗（取代原本的「測試」：開不起來時一樣顯示原因與 SDK 下載按鈕）（[`kinect_preview_window.cpp`](../src/kinect_preview_window.cpp)，
僅 Windows）：左邊彩色影像、右邊深度影像（玩家依骨架位置上色），兩者都疊上追蹤到的骨架
（深度照 SDK 的 `NuiTransformSkeletonToDepthImage` 投影，精確；彩色用標稱焦距，兩顆鏡頭相距幾公分，
是近似對齊），上方列出每個人的編號、骨架位置、距離與「腳是否在畫面內」（腳關節被追蹤到才算）。
Kinect v2 的兩顆鏡頭視野不同（彩色約 84°×54°、深度約 71°×60°），預設把兩個畫面都裁成相同範圍（兩者視野的交集，不再硬切成 4:3，約 1.373:1；以彩色標稱焦距 1060 px、深度 366 px 算出：彩色取約 x 219–1701（1482 px）、y 全部 1080；深度保留完整寬度 x 0–512，只取約 y 25.7–398.3，約 373 列，上下各少約 26 列，約 3°）。兩個面板大小與長寬比相同，沒有黑邊；預覽影像的長邊上限為彩色 640、深度 512，因此彩色約 640×466（每個預覽像素取平均）、深度約 512×373（取最近的像素；玩家依 v2 的身體索引上色）。視窗的面板大小不再固定，而是依畫面的真實長寬比算出（`kinect_preview_layout`，一個純函式，繪圖與骨架座標共用），視窗縮小時兩個面板等比例縮小。彩色的焦距只是標稱值（沒有彩色內參的 API），兩顆鏡頭又相距幾公分（2 公尺處約 1°），所以兩個畫面只是近似對齊。深度畫面上下各約 3° 的範圍遊戲仍然看得到，但預覽不顯示，因此真正的畫面邊緣要看「身體被畫面切到」的警告（被切的邊）。預覽視窗右上角（「往上」左邊）的下拉選單選深度視角，選項與 launcher 的「深度視角」相同、順序也相同：Recompiled、模擬、原始（Kinect v1 時反灰）。開啟時選的是 launcher 目前的視角與 Recompiled 的五個值；在預覽裡改選只影響這個視窗，**不會寫回 launcher 的設定**。launcher 的滑桿在預覽開著時一動，預覽的感應器就改用新值（launcher 每一格把設定交給預覽，`KinectPreviewWindow::set_depth_choice`）；launcher 改了視角時預覽也跟著換。預覽自己開的 v2 用 `KinectSensor::set_depth_settings` 換掉環境變數的視角與調校（在感應器自己的執行緒、下一張深度影像前套用；遊戲不會呼叫），所以預覽裡的骨架與「遊戲拿到的深度」就是那個視角做出來的。遊戲中的「Kinect 調整」視窗借用遊戲的感應器，**永遠不改遊戲的視角**：選單從遊戲自己的視角（`SFR_KINECT_V2_DEPTH_VIEW`）開始，改選只換畫面的裁切，只有遊戲本身是 `ideal` 時才有下面 Recompiled 的四格畫面。各選項顯示：
- **模擬**：兩顆鏡頭共同的範圍（上述裁切），彩色＋深度兩格。
- **原始**：等於以前的「原始」按鈕，關掉裁切，改顯示各鏡頭完整畫面（見下）。
- **Recompiled**：左邊 2×2、右邊一格俯視圖（`kinect_preview_recompiled_layout`，純函式，有單元測試；兩欄同寬，每張畫面都和欄一樣寬、照真實長寬比、靠格子上緣，所以彩色正好在彩色下方、深度正好在深度下方；每列上面一行標題，標題上方留約 8 像素；畫面依視窗高度與三分之二的寬度能放多大就放多大，俯視圖佔其餘寬度（至少三分之一）、與兩列同高；視窗預設 1360×820，可縮放）。
  上排「V2 彩色影像」「V2 深度影像」是實體 v2 的完整畫面（不裁切）＋骨架；下排「模擬 V1 彩色影像」與「模擬 V1 深度影像」**逐像素對齊**，
  深度是**遊戲實際拿到**的 320×240 影像（`game_image()` 的複本，不影響遊戲自己讀的），兩格的骨架都用 v1 鏡頭
  （`NuiTransformSkeletonToDepthImage`）投影，有骨架距離時先換回深度的視角，所以與深度精確對齊。
  模擬 V1 彩色是逐像素重投影（[`kinect_v1_depth_points`](../src/kinect_v2_depth.h)，有單元測試）：深度影像的每個像素以它的深度經 v1 標稱鏡頭反投影到虛擬相機座標，
  再用做出這張影像的同一個視角（影像本身記著平移、z 倍數與水平旋轉）反轉回 v2 感應器座標，整批交給感應器的 `ICoordinateMapper::MapCameraPointsToColorSpace`（拿不到時用典型鏡頭）放進 v2 彩色畫面（`KinectSensor::colour_image_positions`），取最近像素的顏色；沒有深度的像素是黑的。
  所以彩色與深度看到的是同一批點：深度精確、左右在半個像素內；彩色取自 640×360 的預覽影像，兩顆鏡頭的對齊精度就是 mapper 的精度。沒有人時虛擬相機就在 v2 的位置（左右、前後都是 0）、水平、離地 `HEIGHT`，看 v2 視野的正中間；離它超過 4.0 公尺（或不到 0.8 公尺）的地方沒有深度，所以是黑的。
  右邊「俯視位置」是 Recompiled 的俯視圖（[`kinect_ideal_diagram`](../src/kinect_ideal_diagram.h)，純函式，有單元測試 `kinect_ideal_diagram`；
  launcher 的「俯視位置」用同一個函式畫）：實體 v2 在下方往上看，藍色實線、淡藍填色的三角形是它 70.6° 的水平視野，頂點標「實體 v2」；
  橘色虛線、淡橘填色的三角形是虛擬 v1 的 57° 視野，頂點標「虛擬 v1」，兩條橘色點線是 v1 量得到的 0.8 與 4.0 公尺；
  點線橫線是玩家的深度線（虛擬 v1 就在它後面 `DISTANCE` 公尺）；1P 紫、2P 綠（依遊戲的玩家槽位規則 `KinectPlayerSlots`，其他人灰色）的圓在實際位置，
  後面淡色橫帶是那個深度兩台相機都看得到的左右範圍；旁邊寫「v1 到虛擬 v1 的距離（遊戲看到的深度）、v2 到實體 v2 的距離」，
  出了 v1 視野、v1 的 0.8–4.0 公尺或 v2 視野時多一行紅字。左右與深度畫面相同（+x 畫在右邊），所以和旁邊的畫面一致。左邊是從實體 v2 量的深度尺、下方是左右尺（0 在 v2 的中線），每 0.5 公尺一個刻度、每 1 公尺標字並畫一條很淡的橫線；虛擬 v1 的中線上另標 0.8 m 與 4.0 m（從虛擬相機量，遊戲讀得到的範圍）。圖的比例一定包含 0.5–4.5 公尺深、左右各 2.5 公尺。
  虛擬相機的位置**就是**深度合成用的那一個：感應器每做一張深度影像就把 `KinectDepthViewer` 的 ideal 狀態
  （`kinect_ideal_snapshot`：相機 = `kinect_ideal_view` 的原點，玩家 = SpineBase 轉到同一個水平座標）存一份給 `KinectSensor::ideal_snapshot`。
  `KinectDepthViewer` 現在**每種視角都會推進 ideal 狀態**（只有 `ideal` 用它做影像，其他視角的輸出不變），所以預覽或 launcher 畫的永遠是 Recompiled 會放的位置。
  預覽視窗裡「與感應器距離」那行在 Recompiled 時是交給遊戲的骨架距離（虛擬相機），實體 v2 的距離看俯視圖。

原始視角關掉裁切，改顯示各鏡頭完整畫面，面板各自照畫面的長寬比：彩色 1920×1080（16:9）縮成 640×360（每 3×3 像素取平均）；深度 512×424（約 1.208:1）維持原大小，兩個面板同高、靠上對齊，寬度依長寬比（放不下時等比例縮小），沒有黑邊。這只影響預覽的顯示，只保留在該視窗開著的期間，不影響遊戲拿到的深度、骨架或錄影。裁切的計算只在一個純函式（`kinect_v2_crop`）裡，像素轉換、骨架座標與畫面矩形都用它。骨架位置由感測器自己的
`ICoordinateMapper`（`MapCameraPointsToColorSpace`／`MapCameraPointsToDepthSpace`，用感測器出廠的兩顆鏡頭校正）換算到
彩色與深度影像，再照裁切與縮放（原始模式則只有縮放）移進預覽；拿不到 mapper，或 mapper 回答 -infinity（放不進影像）時，改用 v2 的典型鏡頭參數估計（近似對齊）。
log 的 `NATIVE_KINECT_MAPPER model=v2 result=…` 說明有沒有拿到 mapper，每 5 秒的 `NATIVE_KINECT model=v2` 行在預覽開著時多
`preview_mapped_points=`（mapper 放的點）與 `preview_estimated_points=`（鏡頭估計的點）。影像串流在預覽第一次要影像時
才開啟，預覽約 3 秒沒再要就關閉（log：`NATIVE_KINECT_IMAGE model=v2 open …`／`closed`；開著時每 5 秒的
`NATIVE_KINECT model=v2` 行會多 `preview_colour_frames=`、`preview_depth_frames=`），遊戲本身不會開啟它們
（[`kinect_v2_image.cpp`](../src/kinect_v2_image.cpp)，有單元測試 `kinect_v2_image`）。
還沒能玩的時候，最上面一行依序列出四個檢查：執行環境（SDK）、感應器、資料傳送、人體追蹤，
各標 ✓（通過）、✗（失敗）或 …（等待中），下一行說明第一個失敗的步驟要怎麼處理
（[`kinect_readiness`](../src/kinect_preview.h)）。每一步都要真的在運作才算通過：感應器開了但
1.5 秒沒有傳來骨架或影像、或開啟 3 秒都沒有任何資料，「資料傳送」就算失敗；有資料但沒有人，
「人體追蹤」失敗並提示站位。四項都通過後才改回顯示機型、人數和角度。
右上角的「往上」「往下」（或 ↑↓）轉動感應器，邊看邊調到全身入鏡。SDK 一個程式只能開一次 Kinect，所以
按「開始遊戲」會先關閉預覽，把 Kinect 讓給遊戲。

**身體被畫面切到（Kinect v2）**：v2 對每個追蹤到的人回報被畫面哪幾個邊切到（`IBody::get_ClippedEdges`，即 SDK 的 `FrameEdges`）。
預覽（含遊戲中的調整視窗）在每個被切到的人下面多一行橘紅警告，例如「身體被畫面切到：上方，請往後退，或把感應器角度往上」，沒被切到就不顯示。
邊是**畫面**的邊，和預覽畫出的影像一致（未鏡像，也是 Microsoft 範例 BodyBasics 的畫法）：影像左邊是感應器的左邊，玩家面對感應器時就是玩家的右邊，
所以畫面左邊被切到要請玩家往**自己的左邊**移動、右邊被切到往自己的右邊（上、下被切到請往後退或調感應器角度；左右同時或上下同時被切到只能往後退）。
log 在被切到的邊改變時寫 `NATIVE_KINECT_CLIPPED model=v2 body=<槽位> edges=top+left|…|none mask=<位元遮罩>`（每個人每秒最多一行），
每 5 秒的 `NATIVE_KINECT model=v2` 行有人被切到時多 `clipped<槽位>=<邊>`。這只供顯示與記錄，不改變遊戲收到的骨架、深度或任何偵測結果。

遊戲中也開得到同一個視窗（[`kinect_tuner.h`](../src/kinect_tuner.h)）：選單裡調整 Kinect 的選項會向系統要
Kinect 疑難排解（`XamShowNuiTroubleshooterUI`）或 Kinect Guide（`XamShowNuiGuideUI`，結算畫面的 Guide 按鈕），
原本這裡沒有系統畫面、打開就關閉；現在用實體 Kinect 時改成打開「Kinect 調整」視窗，比賽中也一樣（暫停選單裡的 Xbox 選單、玩家離開畫面時的暫停）。它借用遊戲已開的感應器
（不另外開、不關閉），骨架由遊戲讀到後轉交給它（不會搶走遊戲的骨架），彩色與深度影像照常顯示，角度一樣用按鈕或
↑↓ 調整；視窗開在遊戲視窗正中央，按 Esc、手把的 Back（Select）或關閉鈕就回到遊戲；它不會一直蓋在最上層，而是每次叫出時拉到前景（已經開著時再叫一次也是），語言跟系統；webcam 調整視窗也一樣。開著時通知遊戲系統畫面開啟（XN_SYS_UI 1），關閉視窗才通知關閉（0）。
實測遊戲在系統畫面開著時不會自己停住，玩家還能繞過視窗操作遊戲，離開選單後遊戲就不正常了；所以視窗開著時
遊戲等在後面：手把與鍵盤一律讀成放開（`set_input_held`），Kinect 只送一格雙手垂在身側的骨架給遊戲
（`kinect_arms_down`：選單游標要舉手才會出現，手停在按鈕上也不會一直按下），之後不再更新，暫停手勢也不作用；
視窗照常顯示感應器的即時畫面。關閉視窗後一切恢復。用 webcam 時改開「Webcam 調整」視窗（見[攝影機體感](camera-input.md)），只用手把時照舊打開就關閉。log：
`NUI_SYSTEM_UI ... backend=adjustment-window`、`NUI_KINECT_TUNER opened=1 source=kinect`、`NUI_KINECT_TUNER input=held`、
`NUI_KINECT_TUNER input=released`、`NUI_KINECT_TUNER closed`。
