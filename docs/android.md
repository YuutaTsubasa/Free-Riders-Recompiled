# Android 版

遊戲本體與桌面版共用同一份產生碼與執行期，打包成 APK：SDL 2.32.8 的 Java activity 載入
`libSDL2.so` 與遊戲的 `libmain.so`，繪圖用 [Vulkan 後端](vulkan.md)，輸入與聲音走 SDL2
（與 [Linux 版](linux.md) 相同）。

## 需要的東西

- Android SDK（build-tools、platform 35 以上）、NDK（驗證用 29.0.13599879）、JDK 17。
  `ANDROID_HOME` 未設定時使用 `~/AppData/Local/Android/Sdk`（Windows）或 `~/Android/Sdk`。
- `python scripts/bootstrap.py` 取得固定版本的 SDL（`tools/SDL`），並套用 `patches/` 中的
  Plume 修正。
- 產生碼（README 的步驟）。

## 建置

```bash
scripts/build_android.sh
```

逐一建置 `arm64-v8a` 與 `x86_64`（`--abi` 可只選一個），再由 `scripts/package_android.py`
用 SDK 自己的 aapt2、javac、d8、zipalign、apksigner 打包成
`out/android/FreeRidersRecompiled.apk`，並以本機 debug 金鑰簽署。整個過程不經過 Gradle，
不下載任何東西。

## 啟動器

App 一打開是啟動器（`LauncherActivity`，與桌面版同一套頁面，`src/launcher_main.cpp`）：

- **安裝**：「選擇光碟映像檔...」開啟系統的檔案選擇器，選手機上的 Sonic Free Riders `.iso`
  （美版／歐版），啟動器檢查後把光碟檔案複製到 app 的儲存空間，並解碼遊戲程式。需要約 2 GB
  可用空間（光碟檔案 1.7 GB 加上遊戲程式）；安裝完成後映像檔可以刪除。Android 不能選資料夾。
- **著色器包**：「遊戲檔案」分類的「選擇 shaders.pack...」從檔案選擇器複製進來。
- **設定**：音效、略過影片、略過標誌畫面、觸控按鈕、傾斜轉彎等；視窗大小與全螢幕在手機上不適用，
  不顯示。
- **開始遊戲**：啟動器寫入 `settings.env`，在另一個行程（`:game`）開啟 `GameActivity`。
  遊戲中按返回鍵是遊戲的 B（返回）；**長按返回鍵**結束遊戲，回到啟動器。遊戲自行停止時，
  啟動器顯示 `game.log` 的最後幾行。

選到的檔案一律透過檔案選擇器交給 app 的描述元讀取（`/proc/self/fd/N`，用 `dup` 與 `pread`），
不會用路徑重新開啟：重新開啟會回到儲存空間的提供者，而 app 本身沒有權限，讀取會失敗
（先前「找不到 default.xex」就是這個原因）。

中文字型：Android 的中日韓字型是 ImGui 讀不了的可變 CFF2 字型，所以啟動器請 Android 自己
畫出介面用到的幾百個中文字（`LauncherActivity.renderGlyphs`），放進 ImGui 的字型圖集。

## 遊戲檔案

遊戲檔案放在 app 的外部檔案目錄 `/sdcard/Android/data/com.freeriders.recompiled/files`
（啟動器安裝的位置；也可以用 adb 直接放進去）：

| 路徑 | 內容 |
| --- | --- |
| `game/image` | 解碼後的遊戲程式（桌面安裝器的 `game/image`，或 `out/recomp/image-loader`） |
| `game/assets` | 光碟檔案（`game/assets` 或 `private/assets`） |
| `shaders.pack` | 編好的著色器（`python scripts/pack_shaders.py`）；裝置上沒有翻譯器 |
| `settings.env` | 啟動器每次開始遊戲時寫入的執行期設定（每行 `NAME=VALUE`） |
| `game.log` | 執行期的追蹤輸出，每次啟動重寫 |
| `save/` | 存檔（[存檔](saves.md)） |

```bash
adb install -r out/android/FreeRidersRecompiled.apk
adb shell mkdir -p /sdcard/Android/data/com.freeriders.recompiled/files/game
adb push out/recomp/image-loader/. /sdcard/Android/data/com.freeriders.recompiled/files/game/image/
adb push private/assets/. /sdcard/Android/data/com.freeriders.recompiled/files/game/assets/
adb push out/shaders/shaders.pack /sdcard/Android/data/com.freeriders.recompiled/files/
adb shell chmod -R a+rwX /sdcard/Android/data/com.freeriders.recompiled/files
```

用 adb 放檔案時，最後一行讓 app 能讀取它們（它們屬於 shell 使用者）。解除安裝 app 會一併
刪除這個目錄。

## 與桌面版不同的部分

- 執行期在載入 `libmain.so` 時（靜態初始化）就讀取環境變數，所以 `GameActivity` 在載入
  函式庫之前先把 `settings.env` 與遊玩預設值放進環境；`SDL_main`（`src/android_main.cpp`）
  再切換工作目錄、把 stderr 導到 `game.log`，然後呼叫一般的 `main`。
- Plume 在 Android 上直接畫到視窗的 `ANativeWindow`；交換鏈是 RGBA（Android 的表面不提供
  BGRA）。App 在背景時 SDL 暫停事件迴圈（遊戲跟著停住），回到前景時視窗換了新的表面，
  交換鏈在它上面重建；取得或呈現影像失敗時也會重建，並略過那一格。
- NDK 的 libc++ 仍把 `std::jthread`／`std::stop_token` 放在實驗功能，需要
  `-fexperimental-library`。bionic 沒有 `pthread_setaffinity_np`，改用 `sched_setaffinity`。
- `patches/plume-optional-extensions.patch`：`VK_EXT_robustness2` 與
  `VK_KHR_sampler_mirror_clamp_to_edge` 改為選用（模擬器與部分手機驅動沒有），並修正沒有
  null descriptor 時關閉裝置的崩潰。bootstrap 只接受 Plume 上恰好是這些修改。

## 觸控操作

沒有手把時，畫面上有半透明的觸控按鈕（`src/touch_controls.*`，由最後的 blit 著色器畫在遊戲
畫面上；`SFR_TOUCH_CONTROLS=0` 或啟動器的「觸控按鈕」關閉）。iOS 版也預設開啟（Issue #236）。

每個按鈕上寫著 Xbox 手把的名稱（Issue #236）：A、B、X、Y、RT、LT、LB、RB、START，兩個搖桿的
搖桿頭上是 LS、RS。字用按鈕顏色的淺色加暗邊，不用字型：每個字母是幾筆圓頭直線
（`src/touch_labels.cpp`），產生成 `src/shaders/touch_labels.hlsli` 的常數，在 blit 裡算到筆畫的
距離。改字形後執行 `sfr_touch_labels_test --write src/shaders/touch_labels.hlsli`；測試會檢查
檔案沒有過時、字在按鈕內、字母互不相連、720p 時字高至少 12 像素。

啟動器「畫面與效能」的「顯示 FPS」（`SFR_FPS_OVERLAY=1`，預設關，所有平台）在遊戲畫面左上角
顯示每半秒更新的呈現幀率（`src/fps_readout.*`），用同一套筆畫字形由 blit 畫出，數值放在
push constant 的最後一個字。手機沒有視窗標題，所以這是看幀率的方法；桌面的視窗標題統計照舊。

App 圖示（Issue #236）：和 iOS／macOS／Windows 同一張 `packaging/icon/app.png`，
`scripts/package_android.py` 打包時放進 APK 當 `@mipmap/ic_launcher_art`，adaptive icon 的前景
內縮到 74 dp，各種遮罩只會切到四角。minSdk 28，所以不需要舊式 mipmap。

| 位置 | 按鈕 | 用途 |
| --- | --- | --- |
| 比賽中左側 40% 任意處；比賽外只在搖桿的圓圈上 | 浮動搖桿 | 選單中推過一半是十字鍵（每推一次一格）；比賽中是左類比（傾斜轉彎） |
| 右側菱形下／右／左／上 | A（綠）／B（紅）／X（藍）／Y（黃） | 確定、蹲下跳躍；返回、煞車；踢地加速；站姿 |
| X 的左下 | RT（灰） | 使用、搖晃道具 |
| 上方中央 | START（白） | 開始；比賽中暫停 |
| 左／右邊緣中段 | LB／RB（橘，箭頭指向該側） | Power Skill：往左／右出拳 |
| 菱形左上方 | 右類比（青，固定位置） | 比賽中伸手：左右推是往該側伸手拿 Ring（推越多伸越遠），往上是舉起雙手；按住 RT 時擦螢幕。選單中移動手部游標 |
| 下方、搖桿右側 | LT（灰） | 牽手（Tag Race） |

比賽中也可以把手機當方向盤左右傾斜來轉彎（加速度計，5° 以下不動、25° 轉到底；
`SFR_TILT=0` 關閉），按住搖桿時以搖桿為準。
啟動器「控制 › 觸控」的「傾斜前後傾」（`SFR_TILT_LEAN`，預設關閉；Issue #247）再加上前後傾：
手機上緣往外傾（螢幕朝天）等同左類比往上，往自己傾等同往下。比賽中左類比上下只用在特技的轉動
（拉到底時也算「手往後收」），不會加速或煞車（加速是 X 踢地、煞車是 B）。手機的拿法因人而異，
所以比賽開始時（或開始讀到橫向的傾角時）的角度就是中立，離中立 5° 以下不動、25° 傾到底；
在 5° 以內約一秒、超過時約半分鐘慢慢跟上新的拿法。按住 A（蹲下準備跳）時與放開後 0.5 秒內不前後傾，
因為空中技巧看的是起跳時左類比的轉動；沒按 A 就飛出去（滑出跳台邊緣）時則沒有這個保護，
前後傾可能讓搖桿轉一點而算成小小的空中技巧。選單中搖桿不送出類比值，因為那會移動模擬的
Kinect 左手，懸停時會誤選按鈕。右類比則從自己的圓圈開始按才會抓住，之後手指移到別的按鈕上也不會按下它們。

**用手指操作手部游標（Issue #217）**：比賽以外，按在按鈕以外的地方，Kinect 的手部游標就移到
手指下並跟著手指移動；放開後游標留在原處，和真的 Kinect 一樣停留一下就選取。所以「點一下」＝
游標移過去、停住、選取。環狀選單點項目後會出現 → 按鈕，再點它確定。選取造成換頁或開關對話框時，
如果手指已經放開，游標會回到左上角（等待游標的位置），免得停在原處又選到下一頁同位置的按鈕。
**標題畫面**（以及開頭的 SEGA、Sonic Team 標誌）按任何不是按鈕的地方等於 START。細節與量測見
[touch-cursor.md](touch-cursor.md)。

LB／RB 放在左右兩側，是因為比賽畫面左上（Ring、道具）、左下（名次、圈數）、右上（地圖）、右下
（空氣量、速度）都有 HUD，兩側中段是空的（Issue #177）。

## 驗證與限制

在 Android 16 模擬器（x86_64，gfxstream Vulkan）上，遊戲從啟動跑到標題畫面，畫面與聲音正常；
第一次執行建立存檔；用觸控從標題進入主選單、Offline Mode 並轉動選單；切到主畫面再回來後繼續執行。模擬器本身在遊戲執行
幾分鐘後偶爾會整個結束（主機端 gfxstream，Android 內沒有錯誤紀錄）。arm64 版可以建置，但還
沒有在實機上執行。已知限制：

- 傾斜轉彎的方向是依裝置座標推算的，還沒有在實機上確認左右是否正確。
- 只能用 `shaders.pack` 裡的著色器。遊戲開機時就建立全部 468 個著色器（六條賽道與 Grand
  Prix 劇情都沒有出現新的），所以一次開機收集的包就是完整的；APK 內附一份。
- 頁面大小 16 KB 的新款裝置尚未驗證（客體記憶體以 4 KB 為單位對應）。
