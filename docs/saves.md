# 存檔

遊戲只替「已登入的玩家」存檔，玩家則是透過 Kinect 身分辨識對應到帳號。沒有登入時，遊戲會問
「You have not signed in to the gamer profile… you will not be able to save game data」，
之後的紀錄都不會留下。

## 本機玩家（`src/local_profile.*`）

`SFR_PROFILE=1` 讓玩家 1 以一個離線帳號登入（啟動器、`scripts/play.ps1`、
`scripts/play_linux.sh` 與 Android 版都預設開啟；執行期本身預設關閉，讓診斷執行保持原樣）。

- 名稱預設 `Player`，`SFR_PROFILE_NAME` 可改（最多 15 字）。XUID 是由名稱算出的離線 XUID
  （`0xE000…`），所以改名等於換一個帳號、換一份存檔。
- `XamUserGetSigninState`、`XamUserGetName` 回報這個帳號；XGI 的使用者狀態訊息
  （rich presence，只有 Live 會用）接受後丟棄。
- Kinect 模擬的玩家在 `NuiIdentityIdentify` 時被辨識為這個帳號（enrollment 0），遊戲會問
  「Are you Player?」，選「Yes」。

## 存檔內容（`src/content_files.*`）

遊戲以 `XamContentCreateEx` 建立存檔內容並掛成 `sav:`，再用一般的檔案呼叫讀寫
`sav:\SfrAllData.sav`（約 23 MB；另有 `SrnData.sav`、`SrnGhostData.sav` 兩個名稱）。

- 只有一個儲存裝置（硬碟，id 1）：`XamShowNuiDeviceSelectorUI` 直接選它，
  `XamContentGetDeviceData`／`GetDeviceState` 回報它。
- 內容放在 `save/<XUID>/<內容類型>/<檔名>/`（`SFR_SAVE_DIRECTORY` 可改；Android 在 app 的
  外部檔案目錄下）。顯示名稱存在旁邊的 `<檔名>.name`。
- 用啟動器時，存檔資料夾預設是**啟動器旁的 `save`**，不管遊戲的工作目錄在哪裡。啟動器「遊戲檔案」頁的
  「存檔目錄」可以改成別的資料夾（相對路徑從啟動器算起；留空或按「使用預設」回到 `save`），旁邊會顯示
  裡面有沒有存檔（`<XUID>\00000001\SfrAllData.sav\SfrAllData.sav`），「開啟資料夾」直接打開它；
  設定存在 `settings.ini` 的 `save_directory=`，「恢復預設值」不會改動它。換到另一個版本或資料夾時，
  把存檔目錄指到原本的 `save`，或把整個資料夾複製過去。
- 掛載點下的路徑由 `ContentFiles` 處理：建立／開啟／覆寫、讀寫、大小與位置、刪除，
  其餘路徑仍是唯讀的遊戲檔案。檔案代碼在 `0x72300004..0x723FFFFC`。
- 第一次執行時遊戲會問「No previous save data detected. Would you like to create a new save?」，
  選「Create Save Data」。

## 成就（`src/achievements.*`）

遊戲在比賽後等時機以 `XMsgStartIORequest`（XGI 訊息 `0x000B0008`，XUserWriteAchievements）頒發成就，
內容是（玩家, 成就 id）的清單。沒有 Xbox LIVE，所以已登入玩家的成就記在本機：

- 檔案是 `save/<XUID>/achievements.txt`（跟存檔同一個玩家資料夾，`SFR_SAVE_DIRECTORY` 同樣適用）：
  `player=<名稱>`，以及每個成就一行 `<id>=<第一次獲得的 UTC 時間>`，例如 `11=2026-10-24T05:12:33Z`。
  已有的 id 不會被覆寫（時間保持第一次），所以同一個成就重複頒發也沒關係。
- 寫入在獨立的執行緒進行（先寫 `achievements.txt.new` 再改名蓋過），遊戲執行緒不會等磁碟。
  log 會有 `RESULT XUserWriteAchievements count=1 id=11 stored=1 file=...`。
- 遊戲本身不會讀回成就（它沒有匯入成就列舉的函式），自己的進度記在 `SfrAllData.sav`。
- 啟動器的「成就」分頁顯示清單：名稱、說明、成就點數與圖示讀自遊戲 XEX 內的標題資料（以 title id
  `5345084D` 命名的 XDBF 資源：XACH 成就表、各語言 XSTR 字串表、PNG 圖示），讀的是安裝後的
  `image.bin`／`xex_header.bin`。字串依遊戲語言（光碟有英、日、德、法、西、義），「自動」與中文用英文。

## 驗證

```bash
ctest --test-dir out/build/host -R "content_files|achievements"
```

以 `SFR_PROFILE=1` 從標題進入主選單：遊戲建立存檔（23 MB），之後在選單中每次變更都會重新
開啟內容寫入。比賽中 HUD 的 RECORD 在每圈之後更新（沒有登入時一直是 00'00"00）；離開結算
畫面（Return to …）時遊戲寫入紀錄（6752 bytes），下次啟動會讀回。Android 模擬器上第一次執行
同樣建立存檔。

有存檔之後，按 START 會直接進主選單（不再出現 Omochao 的對話框），輸入腳本要跟著調整。

## 測試用：解鎖裝備與其他項目（Issue #385）

要重現需要「已解鎖」內容的問題（例如 Chaos Emeralds＝超級索尼克），不必先玩到 100%：

- 玩家紀錄 S 的位址：`SFR_ACHIEVEMENT_TRACE=<檔案>` 每 1200 個 present 印出
  `ACHIEVEMENT_PROGRESS ... record=0x...`，並把 S 的前 `0x1A40` bytes 寫進那個檔案。
  Windows 與 Linux（WSL）上從空存檔或測試存檔跑發版路線，都是 `0x70137d70`。
- 各個集合的位移見 [achievements.md](achievements.md)；每一格一個 byte，1＝有。
  **已擁有的裝備**是 `S+0x09D8` 起 46 格，**第 38 格是 Chaos Emeralds**（`S+0x09FE`）：
  設成 1，Sonic 的裝備圓環就出現 Chaos Emeralds（Blue Star 往「right」一步），比賽讀 `gr38`。
  空存檔的預設是 0、1、2、4、5、6、17、18、39..43 這幾格。
- `scripts/unlock_pokes.py <record.bin> <位址> <present> [emeralds|gears|parts|tutorials|licenses|all]`
  從上次執行的 S 算出 `SFR_POKE` 的值（只改有變動的 word，其他 bytes 保持遊戲原本的值），
  在角色圓環之前（發版路線約 present 11000）寫入即可。
- `SfrAllData.sav` 有檢查碼（檔頭 `+4` 的 word；`+8` 是每次存檔加一的計數），直接改檔案裡的
  bytes，遊戲會當成沒有存檔而用預設值。要一份「已解鎖」的存檔：讀入一份正常存檔
  （`SFR_PROFILE_NAME` 要跟存檔的 XUID 對得上），在選單裡用 `SFR_POKE` 改 S，再進任一個會存檔的畫面
  （例如 World Grand Prix），遊戲會用正確的檢查碼寫回。

有存檔時按 START 直接進主選單：Free Race 的路線是
`SFR_SAY=start@3400,ok@3800,left@4600,ok@5400,ok@6200,ok@7000,ok@7800,right@8200,ok@8600`
（Offline Mode → Free Race → Normal Race → Dolphin Resort → SONIC → Chaos Emeralds），
並清掉 `SFR_INPUT_SCRIPT`（它的 B 是回答沒有存檔時標題的 Omochao 問題，有存檔時會退回標題）。
