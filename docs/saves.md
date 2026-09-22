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
- 掛載點下的路徑由 `ContentFiles` 處理：建立／開啟／覆寫、讀寫、大小與位置、刪除，
  其餘路徑仍是唯讀的遊戲檔案。檔案代碼在 `0x72300004..0x723FFFFC`。
- 第一次執行時遊戲會問「No previous save data detected. Would you like to create a new save?」，
  選「Create Save Data」。

## 驗證

```bash
ctest --test-dir out/build/host -R content_files
```

以 `SFR_PROFILE=1` 從標題進入主選單：遊戲建立存檔（23 MB），之後在選單中每次變更都會重新
開啟內容寫入。比賽中 HUD 的 RECORD 在每圈之後更新（沒有登入時一直是 00'00"00）；離開結算
畫面（Return to …）時遊戲寫入紀錄（6752 bytes），下次啟動會讀回。Android 模擬器上第一次執行
同樣建立存檔。

有存檔之後，按 START 會直接進主選單（不再出現 Omochao 的對話框），輸入腳本要跟著調整。
