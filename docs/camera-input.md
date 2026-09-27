# 攝影機體感輸入

遊戲本來是 Kinect 專用的，主機端的感測器負責兩件事：給遊戲一張攝影機畫面，
以及回報玩家的骨架。沒有感測器時，[NUI 模擬](main-menu.md)是用手把假裝出一副
骨架；這裡則是讓一台 **webcam** 接手，由畫面推算出真正的身體。

## 三種設定

launcher 的「攝影機」（`SFR_CAMERA`）有三種：

| 設定 | `SFR_CAMERA` | 畫面 | 誰在操作 |
| --- | --- | --- | --- |
| 關閉 | 空白 | 不開攝影機 | 手把 |
| 畫面 | `picture` | 開攝影機，交給遊戲顯示 | 手把 |
| 體感 | `motion` | 開攝影機 | 攝影機看到的身體 |
| Kinect | `kinect` | 實體 Kinect（Windows） | Kinect 追蹤到的身體，見 [實體 Kinect](kinect-sensor.md) |

「體感」時手把不再驅動玩家：兩者共用同一副骨架，不能同時來源不同，
所以在設定頁上是一個選擇而不是兩個開關。

## 從畫面到骨架

1. [`camera_capture`](../src/camera_capture.h)：Windows 走 Media Foundation，
   Linux 走 V4L2，Android 走 NDK 的 Camera2（見下方），要求 640x480。相機給的很少是
   BGRA，所以 YUY2 與 NV12 在 [`convert_camera_pixels`](../src/camera_capture.cpp)
   換算（BT.601），Android 的 YUV_420_888 在 `convert_camera_yuv420` 換算並轉正。
2. [`pose_estimator`](../src/pose_estimator.h)：ONNX Runtime 跑 RTMPose-t，
   輸入 192x256，輸出 SimCC 的兩條機率分布（x 與 y 各自一維，split ratio 2.0），
   取峰值得到 17 個 COCO 關節點。單執行緒，這台機器上一次約 7.5 ms。
   兩肩與兩髖之中最低的分數低於 `SFR_POSE_CONFIDENCE`（預設 0.3）時視為沒看到人——
   只有半個人入鏡比完全沒人更糟。
3. [`pose_smoothing`](../src/pose_smoothing.h)：模型是一張一張獨立讀的，
   站著不動的點在每張之間仍會飄一兩個像素。用 1 Euro filter（Casiez et al., 2012）
   壓掉：靜止時截止頻率低、壓得重，手一動截止頻率立刻拉高、不會延遲。
   `SFR_POSE_SMOOTHING`（靜止時的截止頻率，預設 1 Hz，0 為關閉）與
   `SFR_POSE_SMOOTHING_BETA`（速度的影響，預設 0.05）可調。
4. [`pose_skeleton`](../src/pose_skeleton.h)：17 點換成 NUI 的 20 個關節，
   以**軀幹長度**（兩肩中點到兩髖中點，`pose_torso_length` 0.48 m，與模擬玩家相同）
   為尺度、放在 `pose_distance` 公尺處，補出 NUI 有而 COCO 沒有的脊椎與髖中心。
   原本用肩寬當尺度，但這是滑板遊戲：比賽中側身站時，從正面看兩肩幾乎重疊，
   肩寬趨近於零，換算出來的人會有十幾公尺高（或直接被當成沒看到人）。軀幹長度
   不論面向哪邊、左右傾身都差不多；面對鏡頭時，一般人的軀幹約是肩寬的 1.3 倍，
   和模擬玩家的比例相同，所以選單游標的手感不變。尺度每張只往新值移動十分之一
   （約 0.7 秒跟上），蹲下或彎腰的瞬間不會讓人突然變大；沒看到人時重新量起。
5. [`camera_player`](../src/camera_player.h)：上面幾步跑在自己的執行緒上，
   遊戲那格要骨架時只拿最新一份。找到人之後，第一位玩家的骨架就由攝影機寫入
   （[`nui_hooks`](../src/nui_hooks.cpp) 的 `sub_827707B0`）；還沒找到人以前，
   仍由手把的模擬骨架頂著，所以攝影機開著也不會讓遊戲不能動。

每五秒會印一行 `NATIVE_CAMERA_PLAYER estimates=… tracked=… average_ms=…`，
可以看出模型是不是跟得上，以及有沒有真的看到人。

## 左右與鏡像

攝影機面對玩家，所以**玩家的右手出現在畫面的左半邊**，而那一側正是感測器的
+x——[`nui_skeleton`](../src/nui_skeleton.cpp) 的模擬玩家右肩在 +0.18，選單游標
的中心就在它旁邊（右手 x=0.175 為畫面正中央）。所以模型的右就是 NUI 的右，
名稱直接對過去；`place()` 的 `centre - x` 已經把畫面座標翻成感測器座標了。

（這裡本來多翻了一次：相機骨架的右肩落在 −0.18，和模擬玩家差了 0.36 公尺。
選單游標橫向約 2600 px/m，等於偏左約 975 px，1280 寬的畫面上就是「游標一直
跑到最左邊」。）

有些相機送出來的畫面本身就是鏡像的（手機當視訊鏡頭的 app 多半預設如此）。
一張人的照片**無法**判斷是否鏡像——鏡像的人看起來就是一個人——所以這是一個
設定而不是猜測：launcher 的「攝影機畫面左右相反」（`SFR_CAMERA_MIRROR`）。
設反了的話，舉左手會動到右手，游標也會往反方向跑。開啟時會先把畫面翻回來
再開始量測（座標左右對調，左右標籤也跟著互換，因為模型看到鏡像的人也會把
他的右手叫成左手）。

## 要哪一台相機

`SFR_CAMERA_DEVICE` **存的是相機的名字**，不是編號。原因是清單本身會變：
手機當視訊鏡頭的軟體關掉、擷取卡拔掉，剩下的相機就會重新編號，昨天的 1
今天是別台。開啟時會重新列舉一次，然後照名字找——先整個比對，再比對部分，
都找不到就用第一台。純數字仍然當成清單位置，舊的設定不會壞掉。

launcher 的「使用哪一台」列出的就是同一份清單，旁邊的**測試**按鈕會實際開
那台相機抓一張圖，回答三種結果：

- **有畫面**——這台可以用。
- **開得起來，但沒有畫面**——虛擬相機常見（手機端沒連上、擷取卡沒訊號）。
  這種相機開得成功，所以遊戲不會報錯，只是永遠等不到畫面，看起來就像
  「對著鏡頭揮手都沒反應」。
- **這台攝影機打不開**——被別的程式佔住了。

不開 launcher 也可以問：

```
out/build/host/sfr_camera_probe --list      # 這台電腦有哪些相機
out/build/host/sfr_camera_probe out.bmp     # 抓一張圖存起來
out/build/host/sfr_pose_probe [out.bmp]     # 對相機或那張圖找人
```

## 模型與授權

ONNX Runtime 與 RTMPose 都是 release 產物而不是 repository，所以由
[`scripts/fetch_pose_model.py`](../scripts/fetch_pose_model.py) 以 SHA-256 釘住版本，
解壓到 git 忽略的 `tools/onnx`：

```
python scripts/fetch_pose_model.py            # 這台電腦的 runtime（Windows 或 Linux x64）與模型
python scripts/fetch_pose_model.py --android  # 再加上 Android（arm64-v8a、x86_64）的 runtime
```

- ONNX Runtime 1.30.0：MIT（Microsoft）
- RTMPose-t：Apache 2.0（OpenMMLab；模型壓縮檔裡沒有授權檔，用 MMPose v1.3.2 的 LICENSE）

**發行版會帶上它們**（[`package_release.py`](../scripts/package_release.py)）：
程式旁邊放 `onnxruntime.dll`／`libonnxruntime.so.1`，模型放 `pose/rtmpose.onnx`，
兩份授權條款與 ONNX Runtime 的 ThirdPartyNotices 放 `licenses/`。缺檔時打包會停下，
提示先執行上面的腳本；確定不要體感輸入時可以加 `--no-pose`。Linux 的遊戲執行檔以
`$ORIGIN` 為 RUNPATH，所以會先找自己旁邊的 `libonnxruntime.so.1`。

Android 的 APK 由 [`build_android.sh`](../scripts/build_android.sh) 把各 ABI 的
`libonnxruntime.so` 放進 `lib/<abi>/`、模型放進 `assets/pose/rtmpose.onnx`，
LauncherActivity 每次安裝後第一次啟動時把模型複製到外部檔案資料夾的 `pose/`
（遊戲的工作目錄），和 `shaders.pack` 的做法一樣。

沒有這些檔案時 `SFR_POSE_AVAILABLE` 為 OFF，「畫面」仍然可用，「體感」則會印
`NATIVE_CAMERA_PLAYER motion=0 reason=no-pose-model` 並退回手把。

## Android：手機自己的鏡頭

[`camera_capture_android.cpp`](../src/camera_capture_android.cpp) 用 NDK 的 Camera2
與 AImageReader 讀 YUV_420_888（最接近 640x480 的尺寸）：

- **清單**：系統列出的每一顆鏡頭，名字是「Back camera 0 (78°)」這樣：朝向、系統的
  鏡頭編號、長邊的視角（由感測器寬度與最短焦距算出）。視角越大，玩家可以站得越近。
  有些手機把超廣角藏在「邏輯鏡頭」裡、不單獨列給 App，那種手機只選得到主鏡頭。
- **權限**：第一次開鏡頭時才用 SDL 跳出系統的相機權限詢問（launcher 的「測試」
  按鈕就會觸發），從不開攝影機的玩家不會被問。
- **轉正**：手機感測器是橫著裝的，依 `SENSOR_ORIENTATION`、鏡頭朝向與螢幕目前的
  轉向（SDL 的 display orientation）算出順時針要轉幾度
  （[`camera_upright_rotation`](../src/camera_capture.h)），轉換時一併轉好。
  Camera2 給的前鏡頭畫面本身不是鏡像的，所以不需要打開「畫面左右相反」。

**建議的擺法**：手機接電視（USB-C 轉 HDMI 或投影），放在電視旁、背面超廣角朝向玩家；
或把手機立在電視旁用前鏡頭。只看手機螢幕的話，站到能全身入鏡的距離就太小了。

這台開發機沒有 Android NDK 與手機：轉換、旋轉與轉正角度有單元測試
（`camera_capture` CTest），NDK 那一層還沒在實機上跑過。

## 從畫面推回轉身的深度

滑板是側身站的：腳朝側面，上半身稍微轉向螢幕；**完全轉正面對螢幕，遊戲判定為停止**。
遊戲從骨架的深度看這件事（一肩在另一肩後面多遠），而 webcam 沒有深度：如果所有
關節都放在同一個距離，兩肩永遠等距，遊戲會以為玩家一直轉正、一直在停。

所以 [`pose_to_joints`](../src/pose_skeleton.h) 只為這件事推回深度：

- **轉多少**：肩膀的真實寬度已知（軀幹給了尺度，肩寬 0.36 m），看起來越窄，兩肩的
  前後差就越大：前後差 = √(真寬² − 看到的寬²)。側身時差一個肩寬，完全轉正時為零。
  髖部用同樣的方法（寬 0.2 m），手臂與腿跟著各自那一側的肩或髖。
- **哪一邊在前**：前面那一側較近。鏡頭通常比腳高，較近的腳在畫面上較低，所以腳踝
  較低的那一側是前腳：左腳在前是 Regular、右腳在前是 Goofy。兩腳踝相差不到軀幹的
  1/16 時沿用上一次的判斷，所以只有真的轉身（switch）才會換邊。
- 前腳還沒判斷出來以前不給深度。面對鏡頭（選單）時看到的肩寬就是真寬，前後差為零，
  所以選單操作完全不受影響。

這仍是從 2D 推回來的近似：鏡頭與腳同高時腳踝看不出前後，手往前伸的距離也推不回來。

## 用身體比賽（實驗性）

預設在比賽中仍由手把操作，因為 webcam 沒有深度：放在電視旁的 webcam，看得到
側身玩家的前後傾（畫面左右）、蹲下與跳躍（畫面上下），看不到朝螢幕投擲這類
直直朝鏡頭的動作。感應器位置（側放、斜放）只適用於有深度的 Kinect：webcam 側放時，
遊戲需要的其中一個方向剛好變成它看不到的深度。launcher 的「比賽也用身體操作」
（`SFR_CAMERA_RACE=1`）打開後，第一次找到人起，比賽就和 [實體 Kinect](kinect-sensor.md)
一樣交給遊戲自己的手勢判斷器讀攝影機的骨架：左右傾身、蹲下、跳躍這類在畫面上
看得出來的動作有機會辨識，朝螢幕推手之類需要深度的動作則不行。尚未實際試玩。

## 兩位玩家

體感開著時第一位玩家是攝影機，這時**第一支手把**就是第二位玩家。
詳見 [兩位玩家](two-players.md)。

## 還沒做的

- 攝影機的畫面還沒接到遊戲自己的 NUI 影像串流（`82768C40`），所以「畫面」
  目前只是把相機打開。
- 真正的 Kinect 感測器見 [實體 Kinect](kinect-sensor.md)（Windows，`SFR_CAMERA=kinect`）。
