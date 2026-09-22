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
- **設定**：音效、略過影片、觸控按鈕、傾斜轉彎、多核心執行等；視窗大小與全螢幕在手機上不適用，
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
畫面上；`SFR_TOUCH_CONTROLS=0` 關閉）：

| 位置 | 按鈕 | 用途 |
| --- | --- | --- |
| 左側 40% 任意處 | 浮動搖桿 | 選單中推過一半是十字鍵（每推一次一格）；比賽中是左類比（傾斜轉彎） |
| 右側菱形下／右／左／上 | A（綠）／B（紅）／X（藍）／Y（黃） | 確定、蹲下跳躍；返回、煞車；踢地加速；站姿 |
| X 的左下 | RT（灰） | 使用、搖晃道具 |
| 上方中央 | START（白） | 開始；比賽中暫停 |

比賽中也可以把手機當方向盤左右傾斜來轉彎（加速度計，5° 以下不動、25° 轉到底；
`SFR_TILT=0` 關閉），按住搖桿時以搖桿為準。選單中搖桿不送出類比值，因為那會移動模擬的
Kinect 左手，懸停時會誤選按鈕。

## 驗證與限制

在 Android 16 模擬器（x86_64，gfxstream Vulkan）上，遊戲從啟動跑到標題畫面，畫面與聲音正常；
第一次執行建立存檔；用觸控從標題進入主選單、Offline Mode 並轉動選單；切到主畫面再回來後繼續執行。模擬器本身在遊戲執行
幾分鐘後偶爾會整個結束（主機端 gfxstream，Android 內沒有錯誤紀錄）。arm64 版可以建置，但還
沒有在實機上執行。已知限制：

- 傾斜轉彎的方向是依裝置座標推算的，還沒有在實機上確認左右是否正確。
- 只能用 `shaders.pack` 裡的著色器。遊戲開機時就建立全部 468 個著色器（六條賽道與 Grand
  Prix 劇情都沒有出現新的），所以一次開機收集的包就是完整的；APK 內附一份。
- 頁面大小 16 KB 的新款裝置尚未驗證（客體記憶體以 4 KB 為單位對應）。
