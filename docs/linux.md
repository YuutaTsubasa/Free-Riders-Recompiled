# Linux 版

Linux 上遊戲本體（`sfr_cpu_diagnostic`）與 Windows 共用同一份產生碼與執行期，只換掉作業系統
相關的部分。繪圖一律用 [Vulkan 後端](vulkan.md)，視窗、輸入與聲音走 SDL2。

## 需要的套件

在 Ubuntu 22.04（含 WSL2／WSLg）驗證過：

```bash
sudo apt install -y build-essential clang-15 cmake ninja-build pkg-config libsdl2-dev libx11-dev mesa-vulkan-drivers vulkan-tools
```

產生碼需要 Clang（clang-15 或更新版）；標準函式庫用系統的 libstdc++。

## 建置與遊玩

先在 Windows 或 Linux 上照 README 準備好 `out/recomp/image-loader`、`private/assets` 與產生碼，
再執行：

```bash
python3 scripts/bootstrap.py
scripts/build_linux.sh --diagnostic out/recomp/diagnostic
scripts/play_linux.sh --skip-movies
```

建置目錄裡也有啟動器 `FreeRidersRecompiled`：與 Windows 版同一套安裝與設定頁面（SDL2 視窗，
ImGui 的 SDL 繪製後端）。檔案選擇對話框用 zenity 或 kdialog（有安裝時）；沒有時可以在
「遊戲檔案」直接輸入路徑、把光碟映像檔拖到視窗上，或當成第一個參數傳給啟動器。遊戲以子行程
啟動，存檔在啟動器旁的 `save/`；啟動器上層沒有專案目錄時，使用它旁邊的 `shaders.pack`
（「遊戲檔案」分類可以選一個複製過來）。

建置目錄預設是 `~/sfr-build`（`SFR_LINUX_BUILD` 可改）。放在 WSL 的 Linux 檔案系統裡，比放在
`/mnt/c` 快很多。`play_linux.sh` 的環境變數和 Windows 的 `scripts/play.ps1` 相同；有
`out/shaders/shaders.pack` 時會自動使用，不需要翻譯器與 DXC（見 [著色器包](vulkan.md#著色器包)）。
包裡沒有的著色器仍在執行時用 XenosRecomp 翻譯，並以 dxc-bin 的 `dxc-linux` 編成 SPIR-V。

## 應用程式選單

Linux 啟動器每次啟動時，照 freedesktop 的 Desktop Entry 規格維護一個使用者自己的選單項目
（`src/linux_desktop_entry.cpp`，Issue #365）：

| 檔案 | 位置 |
| --- | --- |
| 選單項目 | `$XDG_DATA_HOME/applications/com.freeriders.recompiled.desktop` |
| 圖示（啟動器畫的同一個，256×256 PNG） | `$XDG_DATA_HOME/icons/hicolor/256x256/apps/com.freeriders.recompiled.png` |

`$XDG_DATA_HOME` 沒設定（或不是絕對路徑）時是 `~/.local/share`。

- `Exec=`、`TryExec=` 是啟動器現在的絕對路徑（`/proc/self/exe`），`Path=` 是它的資料夾；
  路徑有空白或特殊字元時照規格的 `Exec` 引號規則寫。資料夾搬走後，下一次啟動會改寫；
  內容沒變就不重寫。資料夾被刪掉時，`TryExec` 讓選單自動藏起這個項目。
- 啟動器和遊戲的視窗都用同一個 id `com.freeriders.recompiled` 當 X11 的 `WM_CLASS` 與
  Wayland 的 app ID（`SDL_VIDEO_X11_WMCLASS`、`SDL_VIDEO_WAYLAND_WMCLASS`、`SDL_APP_ID`，
  玩家自己設了其中任何一個就不動），項目的 `StartupWMClass` 也是它，所以桌面會把執行中的
  視窗歸到這個項目、用它的圖示。
- 寫入或移除後，有安裝 `update-desktop-database` 時會對 applications 資料夾跑一次；沒有也不算失敗。
- **一般 › 桌面 › 加入應用程式選單**（`settings.ini` 的 `application_menu`，預設開）關閉時移除兩個
  檔案。啟動器只改動自己寫的檔案：選單項目要有 `X-FreeRidersRecompiled-Managed=true`，圖示要有
  同樣內容的 PNG 文字區塊；同名但不是它寫的檔案不會被覆寫或刪除。
- 在 AppImage、Flatpak、Snap 裡執行時（有 `APPIMAGE`、`FLATPAK_ID`、`SNAP` 環境變數或
  `/.flatpak-info`）這些套件有自己的桌面整合，啟動器不寫，也不顯示這個開關。Windows、macOS、
  Android 不做這件事。

## 與 Windows 不同的部分

| 部分 | Windows | Linux |
| --- | --- | --- |
| 繪圖 | D3D12（預設）或 Vulkan | Vulkan |
| 視窗 | Win32 視窗 | SDL 視窗（Plume 建立 Vulkan 介面時就需要它，第一次呈現才顯示）；Alt+Enter 全螢幕 |
| 輸入 | XInput、PlayStation HID、鍵盤 | SDL GameController（含 PlayStation 手把）、SDL 鍵盤狀態；按鍵配置相同（[輸入](native-input.md)） |
| 聲音 | XAudio2 | SDL 音訊佇列，48 kHz 立體聲；超過約四分之一秒待播時丟棄，和 XAudio2 版相同 |
| 執行緒 | Win32 執行緒 | pthread（16 MB 堆疊、暫停閘門、`pthread_setaffinity_np`） |
| 核心物件等待 | Win32 事件／semaphore | `portable_waitables`：共用 mutex 與 condition variable |
| 檔案 | Win32 檔案 API | 不分大小寫逐層查找、`O_NOFOLLOW`、`pread` |
| 介面語言 | `GetUserDefaultUILanguage` | `LC_ALL`、`LC_MESSAGES`、`LANG` 的語言與地區（`C` 當作美式英文；中文要有地區才分得出繁簡） |
| 國家 | `GetUserDefaultGeoName` | `LC_ALL`、`LC_ADDRESS`、`LANG` 的地區（`C` 當作 US） |
| 顯示模式 | `EnumDisplaySettings` | `SDL_GetCurrentDisplayMode`（WSLg 不回報更新率，當作 60 Hz） |
| Winsock | `WSAStartup` | 不需要啟動；回報 Winsock 2.2 的結果 |
| `RtlNtStatusToDosError` | ntdll 的對照表 | 檔案與等待會用到的狀態碼對照表 |

Windows 專用的只剩主機端取樣分析（`SFR_HOST_PROFILE`、`SFR_MAIN_PROFILE`），以及直接和 Windows
比對的幾個測試（原生光柵、執行緒、具名同步、非同步讀取、資產檔案、Winsock）。

## 驗證

```bash
scripts/build_linux.sh
ctest --test-dir ~/sfr-build -j 8
```

WSL2 Ubuntu 22.04、clang 15、SDL 2.0.20、Mesa llvmpipe（軟體繪圖）上 75 項測試全部通過。
遊戲本體在同一環境從啟動跑到標題畫面（略過影片時 900 格約 21 秒），並進入 Free Race 比賽，畫面與 Windows 相同，
聲音經 WSLg 的 PulseAudio 輸出。
