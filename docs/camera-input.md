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

「體感」時手把不再驅動玩家：兩者共用同一副骨架，不能同時來源不同，
所以在設定頁上是一個選擇而不是兩個開關。

## 從畫面到骨架

1. [`camera_capture`](../src/camera_capture.h)：Windows 走 Media Foundation，
   Linux 走 V4L2，要求 640x480。相機給的很少是 BGRA，所以 YUY2 與 NV12
   在 [`convert_camera_pixels`](../src/camera_capture.cpp) 換算（BT.601）。
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
   以肩寬 `pose_shoulder_half_width` 為尺度、放在 `pose_distance` 公尺處，
   補出 NUI 有而 COCO 沒有的脊椎與髖中心。
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
解壓到 git 忽略的 `tools/onnx`。兩者都**沒有**隨本專案散布：

- ONNX Runtime 1.30.0：MIT（Microsoft）
- RTMPose-t：Apache 2.0（OpenMMLab）

要把它們放進發行版的話，發行版就必須一併帶上這兩份授權條款。
沒有這些檔案時 `SFR_POSE_AVAILABLE` 為 OFF，「畫面」仍然可用，「體感」則會印
`NATIVE_CAMERA_PLAYER motion=0 reason=no-pose-model` 並退回手把。

## 兩位玩家

體感開著時第一位玩家是攝影機，這時**第一支手把**就是第二位玩家。
詳見 [兩位玩家](two-players.md)。

## 還沒做的

- 攝影機的畫面還沒接到遊戲自己的 NUI 影像串流（`82768C40`），所以「畫面」
  目前只是把相機打開。
- Android 還沒有 Camera2 的擷取實作。
- 真正的 Kinect 感測器（走 USB）沒有做，手邊沒有機器可以試。
