# 攝影機體感輸入

遊戲本來是 Kinect 專用的，主機端的感測器負責兩件事：給遊戲一張攝影機畫面，
以及回報玩家的骨架。沒有感測器時，[NUI 模擬](main-menu.md)是用手把假裝出一副
骨架；這裡則是讓一台 **webcam** 接手，由畫面估算身體的相對 3D 姿勢。
這不是 Kinect 的深度量測：單眼模型推算關節前後關係，不能量出玩家離相機的真實距離。

## 三種設定

launcher 的「攝影機」（`SFR_CAMERA`）有三種 webcam 設定，Windows 另有實體 Kinect：

| 設定 | `SFR_CAMERA` | 畫面 | 誰在操作 |
| --- | --- | --- | --- |
| 關閉 | 空白 | 不開攝影機 | 手把 |
| 畫面 | `picture` | 開攝影機，交給遊戲顯示 | 手把 |
| 體感 | `motion` | 開攝影機 | 攝影機姿勢，手把操作時優先接手 |
| Kinect | `kinect` | 實體 Kinect（Windows） | Kinect 追蹤到的身體，見 [實體 Kinect](kinect-sensor.md) |

「體感」保留原本的手把與鍵盤操作。每次提交完整骨架時只選一個來源，
避免攝影機與手把同時拉動同一位玩家；接手規則見下方「兩位玩家」。

## 從畫面到骨架

1. [`camera_capture`](../src/camera_capture.h)：Windows 走 Media Foundation，
   Linux 走 V4L2，要求 640x480。相機給的很少是 BGRA，所以 YUY2 與 NV12
   在 [`convert_camera_pixels`](../src/camera_capture.cpp) 換算（BT.601）。
2. [`pose_estimator`](../src/pose_estimator.h)：ONNX Runtime 預設跑 OpenCV Zoo 的
   MediaPipe 人物偵測與姿勢模型。先找出身體並裁切、旋轉，再估算 33 個關節的
   畫面位置與以髖部為中心的相對 3D 座標。選出對應的 17 個 COCO 關節，保留
   現有的 `PoseLandmarks` 介面，附上 XYZ；後面再轉成遊戲的 20 個 NUI 關節。
   偵測與姿勢推論都在同一個攝影機 worker 上執行，ONNX Runtime 使用單執行緒。
   兩肩與兩髖之中最低的分數低於 `SFR_POSE_CONFIDENCE`（3D 預設 0.5，舊 RTMPose 為 0.3）時視為沒看到人——
   只有半個人入鏡比完全沒人更糟。
3. [`pose_smoothing`](../src/pose_smoothing.h)：模型是一張一張獨立讀的，
   站著不動的關節仍會抖動。畫面 XY 與相對 3D 的 X、Y、Z 都使用
   1 Euro filter（Casiez et al., 2012）：靜止時截止頻率低、平滑較強，
   手一動截止頻率就提高，減少延遲。
   `SFR_POSE_SMOOTHING`（靜止時的截止頻率，預設 1 Hz，0 為關閉）與
   `SFR_POSE_SMOOTHING_BETA`（速度的影響，預設 0.05）可調。
4. [`pose_skeleton`](../src/pose_skeleton.h)：17 點換成 NUI 的 20 個關節，
   以 3D 肩寬統一縮放到遊戲的肩寬（`pose_shoulder_half_width`），保留四肢的
   前後差異；髖中心固定在 `pose_distance`（2.5 公尺），補出 NUI 有而 COCO
   沒有的脊椎、髖中心等關節。這個 2.5 公尺是遊戲用的錨點，並非量測距離。
   手向鏡頭伸出時，交給遊戲的手部 Z 應減少；收回身旁時則回到軀幹附近。
5. [`camera_player`](../src/camera_player.h)：上面幾步跑在自己的執行緒上，
   遊戲那格要骨架時只拿最新一份，並取得最後有效姿勢的時間。
   [`nui_hooks`](../src/nui_hooks.cpp) 的 `sub_827707B0` 依姿勢新鮮度與手把操作
   選擇第一位玩家的來源；還沒找到人或追蹤中斷時仍能用手把。

每五秒會印一行 `NATIVE_CAMERA_PLAYER estimates=… tracked=… average_ms=…`，
可以看出模型是不是跟得上，以及有沒有真的看到人。

## 骨架 Debug 視窗（Windows）

launcher 選擇「攝影機 → 體感」後，可開啟「骨架 Debug 視窗」
（英文介面為 **Skeleton debug window**）。預設關閉，設定存為
`settings.ini` 的 `camera_debug=0/1`，會在下次啟動遊戲時套用；遊戲已經
執行中時，必須重新啟動才會套用變更。

這是一個獨立視窗，**只顯示已交給遊戲的 20 個骨架關節，不顯示攝影機影像**。
正面視圖使用 X/Y，側面視圖使用 Z/Y，單位為公尺，左右身體以不同顏色區分。
側面視圖呈現模型估算的相對 3D 姿勢，髖中心固定在 2.5 公尺；不代表真正測得的深度。
視窗會區分 Camera／Controller 來源、等待姿勢、短暫沿用上次姿勢及遊戲更新停滯。
手把接手時，視窗也切換成實際送入遊戲的手把模擬骨架。

Debug 視窗讀取遊戲提交的骨架快照，不會增加攝影機擷取或模型推論，也不會
改變體感輸入。關閉 Debug 視窗不會停止遊戲或攝影機追蹤；若要重新開啟視窗，
請重新啟動遊戲。

不經 launcher 時，可設定 `SFR_CAMERA=motion` 與 `SFR_CAMERA_DEBUG=1`。
launcher 只有在「體感」與 Debug 開關都開啟時才會傳入 `SFR_CAMERA_DEBUG=1`，
其他情況一律傳入 `0`，覆蓋外部環境中殘留的設定。初版僅支援 Windows；其他
平台保留設定檔中的偏好，但不顯示此開關，啟動時也會將 Debug 設為 `0`。

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

[`scripts/fetch_pose_model.py`](../scripts/fetch_pose_model.py) 以 SHA-256 釘住
ONNX Runtime、模型與 MediaPipe 授權檔，放在 git 忽略的 `tools/onnx`：

```powershell
python scripts/fetch_pose_model.py                 # 預設：MediaPipe 3D + ONNX Runtime
python scripts/fetch_pose_model.py --verify-only   # 只核對所選下載檔，不下載、不解壓
python scripts/fetch_pose_model.py --model rtmpose  # 選用舊的 RTMPose 2D 模型
python scripts/fetch_pose_model.py --model rtmpose --verify-only
python scripts/fetch_pose_model.py --platform linux  # Linux x64 的 ONNX Runtime（同樣放在 tools/onnx/onnxruntime）
python scripts/fetch_pose_model.py --android         # 另外下載 Android（arm64-v8a、x86_64）的 ONNX Runtime
```

Linux 的遊戲執行檔以 `$ORIGIN` 為 RUNPATH，會先找自己旁邊的 `libonnxruntime.so.1`。
Android 的 runtime 解壓到 `tools/onnx/onnxruntime-android`（AAR 內沒有授權檔，授權與
ThirdPartyNotices 取自同版本的 Linux 套件），CMake 依 ABI 連結。

預設下載 `tools/onnx/mediapipe/` 內的
`pose_estimation_mediapipe_2023mar.onnx`、`person_detection_mediapipe_2023mar.onnx`
及 `LICENSE`。來源固定在 OpenCV Zoo commit
`47534e27c9851bb1128ccc0102f1145e27f23f98`；ONNX Runtime 仍解壓至
`tools/onnx/onnxruntime/`。下載後重新執行 CMake configure，讓建置找到依賴。

`SFR_POSE_MODEL` 可指定姿勢模型路徑；MediaPipe 的人物偵測模型預設讀取同一
資料夾的 `person_detection_mediapipe_2023mar.onnx`，也可用 `SFR_POSE_DETECTOR`
指定。若要沿用 RTMPose，下載 `--model rtmpose` 後，明確設定
`SFR_POSE_MODEL` 為 `tools/onnx/rtmpose/end2end.onnx` 的完整路徑。
RTMPose 仍使用 17 點 COCO、192x256 輸入與 SimCC XY 解碼，沒有相對 3D
座標，會沿用固定深度骨架；只下載舊模型不會切換預設的 MediaPipe 路徑。

從 v0.2.0 起，Windows 發行包包含 MediaPipe 模型與 ONNX Runtime，並附上授權及
第三方通知；不需要另外下載。`scripts/package_release.py windows --camera` 會檢查
模型、執行庫與授權檔是否齊全。Linux 與 Android 的預建包目前未啟用 Camera 體感；
Linux 自行建置可用 `SFR_ONNX_ROOT` 指定 Linux ONNX Runtime 安裝目錄。

依賴授權：

- ONNX Runtime 1.30.0：MIT（Microsoft）
- MediaPipe 人物偵測與姿勢模型：Apache 2.0（OpenCV Zoo 所附授權）
- RTMPose-t：Apache 2.0（OpenMMLab）

沿用 RTMPose 的自訂發行包也必須附上其授權條款與通知。
沒有這些檔案時 `SFR_POSE_AVAILABLE` 為 OFF，「畫面」仍然可用，「體感」則會印
`NATIVE_CAMERA_PLAYER motion=0 reason=no-pose-model` 並退回手把。

## 兩位玩家

體感開著時攝影機是 **1P 的額外輸入**，Controls 設定的 1P／2P 手把不會重新分配。
操作 1P 手把（包含鍵盤映射）時立即改用手把；停止操作 1.5 秒後，若仍有新鮮的
攝影機姿勢，就恢復體感。類比死區內的飄移不會切換來源。攝影機超過 0.5 秒沒有
有效姿勢時退回手把；追蹤恢復且手把已閒置時才回到體感。

骨架跟隨遊戲實際的玩家綁定，不假設第一個骨架槽就是 1P。Debug 視窗顯示遊戲
收到的 1P 骨架，並標示目前由 Camera 或 Controller 控制；不顯示攝影機影像。
詳見 [兩位玩家](two-players.md)。

## Camera 比賽動作（2026-09-27）

進入比賽或從手把／失去追蹤回到 Camera 後，先保持側身滑行的自然站姿約一秒，讓軀幹與腿部完成
短暫站姿校正。Debug 仍顯示原本送給遊戲的骨架；比賽轉向另外以左右傾斜換算
遊戲需要的深度影像比例欄位，避免沒有 Kinect 深度像素時的 `0/0` 被判成向右傾。

一般前傾加速與消耗 Air 的 Kick Dash 是不同動作。先前把兩者都稱為 Boost 的說明不正確。
Kick Dash 使用原版腳部蹬地動作，像滑板的後腳向後蹬；前傾不會代替 Kick Dash。
Camera 原版腳部判定通過後，也需保留遊戲共用的優先旗標，否則後續篩選會把準備／出腳訊號清掉。
這不自動產生腳部動作，也不送出剎車訊號；揮手與蹲跳的防誤觸保護繼續生效。
若腳踝和膝蓋相對髖部已有連續、同向的前後移動，且另一腳相對穩定，Camera 會容許
平衡用的手部動作，不再因此取消 Kick Dash 的 Ready。確認過的伸腳姿勢可保持；收回後
有短暫釋放窗口，回到站姿、失去追蹤或進入蹲跳會結束這個例外。單純揮手或單筆腳部
雜訊不會取得例外。Kick Dash 仍需原版的前後蹬地條件，單純把腳抬高不保證進入 Ready。

一般前傾加速：先收手、回正並停一下，再讓肩膀比腰部更靠近攝影機，腿部保持站立。
需要比校正站姿額外朝攝影機傾約 18 度並維持約 0.2 秒（實際操作可停約半秒），回正會停止；蹲下時不觸發
前傾加速。Camera 的蹲跳使用髖部到較低腳踝的高度縮短／恢復判定，保留模型的真實 XYZ，
不把固定髖部原點誤當成沒有動作：下蹲維持一下，再明顯起身會送出一次跳躍。
深蹲的起跳改以經連續觀測確認的最低點為基準，不必等到幾乎完全站直；最低點與
起升都需相鄰的新骨架資料支持，避免單筆腿部高低估計造成假跳。起身後需回到站姿
才會重新允許下一次蹲跳。
這是 RGB 姿勢模型的操作換算，不是 Kinect 深度影像或絕對離地高度量測。

剎車保留原版動作：側身滑行，轉成正面朝向感測器時剎車；一般站姿不是正面站著。
Camera 需肩膀與骨盆都朝正面（約 30 度內）持續約 0.2 秒，才放行原版膝蓋與剎車
判定；轉離約 40 度即解除。這避免膝蓋單獨被模型估成正面就煞車。蹲下與起身期間
不讓 Side／剎車搶走跳躍。不使用額外張開雙腳的替代剎車動作。

揮手／投擲優先於加速與剎車：手腕相對同側肩膀快速移動，或手腕舉過肩膀時，
暫停 Camera 的前傾加速、Kick Dash 與 Side／剎車，並清掉舊的踢地準備狀態。
收手後先回正，再重新前傾才能一般加速；回到側身滑行姿勢，再轉向畫面才能剎車。
回正／側身需連續新骨架確認，避免投擲收尾或單筆抖動變成延後加速、剎車。
這個優先順序不改 2P 手把；完成校正後的一般轉向與蹲跳繼續使用原本流程。

Camera 持續動作以主機單調時鐘計時；遊戲計時欄位實際可為 `1`，不能直接當成秒。
判定只隨新的姿勢觀測推進，攝影機重複上一張結果不會累積成長按；新的第一張
前傾／下蹲也不能把之前等待的時間當成持續動作。超過 0.25 秒沒有新姿勢會清除
比賽動作與校正，避免停格誤加速。1P 手把接手與 2P 的設定／操作維持原本行為。

測試資料夾的 `Trace-Camera-Race.cmd` 記錄骨架、遊戲身體數值及動作判定狀態變化，
也記錄角色收到的動作旗標、遊戲允許遮罩及動畫狀態，以追查蹲伏已辨識但畫面沒有反應的情況；
不記錄攝影機影像。投擲先執行原版判定；Camera 的上投額外容許前臂斜向上準備：
手腕高過肩膀、前臂朝上約 37 度以上，再朝攝影機揮出。準備姿勢離開後保留最多
0.5 秒，只有新骨架可更新準備；前伸仍需原版的方向與 1 公分位移門檻。
原版成功就不補發，同一次上投只送一次，失去追蹤／切回手把會清除準備。
下投與其他道具仍用原版，沒有更改道具映射；上投與使用者所稱「炸彈」的對應
仍需持有該道具的實機操作確認。
仍需要真人確認操作靈敏度；目前固定的 Camera 門檻不是從遊戲還原的 Kinect 常數。

## 還沒做的

- 攝影機的畫面還沒接到遊戲自己的 NUI 影像串流（`82768C40`），所以「畫面」
  目前只是把相機打開。
- 真正的 Kinect 感測器見 [實體 Kinect](kinect-sensor.md)（Windows，`SFR_CAMERA=kinect`）。
- Android 的遊戲還沒把姿勢模型打包進 APK（手機鏡頭的擷取已經有了，見下方）。

## Android：手機自己的鏡頭

[`camera_capture_android.cpp`](../src/camera_capture_android.cpp) 用 NDK 的 Camera2
與 AImageReader 讀 YUV_420_888（最接近 640x480 的尺寸）：

- **清單**：系統列出的每一顆鏡頭，名字是「Back camera 0 (78°)」這樣：朝向、系統的
  鏡頭編號、長邊的視角（由感測器寬度與最短焦距算出）。視角越大，玩家可以站得越近。
  有些手機把超廣角藏在「邏輯鏡頭」裡、不單獨列給 App，那種手機只選得到主鏡頭。
- **權限**：第一次開鏡頭時才用 SDL 跳出系統的相機權限詢問（launcher 的「測試」
  按鈕就會觸發），從不開攝影機的玩家不會被問。
- **轉正**：手機感測器是橫著裝的，依 `SENSOR_ORIENTATION`、鏡頭朝向與螢幕目前的
  轉向算出順時針要轉幾度（[`camera_upright_rotation`](../src/camera_capture.h)），
  轉換時一併轉好（`convert_camera_yuv420`）。Camera2 給的前鏡頭畫面本身不是鏡像的。

**建議的擺法**：手機接電視（USB-C 轉 HDMI 或投影），放在電視旁、背面超廣角朝向玩家；
或把手機立在電視旁用前鏡頭。

轉換、旋轉與轉正角度有單元測試（`camera_capture` CTest），CI 的 Android launcher
建置會編譯它；實機還沒試過。
