# Sonic Free Riders Native 重編譯設計草案

日期：2026-09-09。狀態：使用者已核准設計並授權實作；尚未產生可玩版本。

## 目標與已確認需求

以工作區 `__ROM__` 中的 Sonic Free Riders Xbox 360 ISO 為輸入，建立 Windows、Linux、macOS、Android、iOS 原生靜態重編譯版本。參考 Marathon Recompiled 與 Unleashed Recompiled 的工具鏈及執行環境設計。

使用者已確認：先支援手把／鍵盤，手機提供觸控與陀螺儀操作。攝影機體感不作為第一版啟動及遊玩的必要条件。操作映射需涵蓋選單、校正流程、轉向、蹬地、跳躍、技巧、道具、暫停；具體遊戲動作與狀態必須由程式分析及實機驗證確定。

原生的驗收定義：遊戲 PowerPC 程式在建置時轉成 C++，再由目標平台編譯器生成 x86-64／ARM64 機器碼。以原生執行环境承接 Xbox 系統服務與圖形／音訊；不以啟動模擬器、串流或只有空視窗的程式作為完成成果。

## 本機調查證據

調查採唯讀方式，從 ISO 讀取 XDVDFS 目錄、XEX 標頭與 SHA-256；未修改 ISO、未解密程式主體、未執行遊戲。根目錄解析結果尚非完整資產完整性檢驗。

| 項目 | 實測值 |
| --- | --- |
| ISO | `__ROM__/Sonic Free Riders (USA, Europe) (En,Ja,Fr,De,Es,It).iso` |
| ISO 大小 | 7,838,695,424 bytes |
| ISO SHA-256 | `f15775c9a0eb1a09920794a169bb904ce5b0e38e05a06bc92b7c59707d964321` |
| 遊戲分割區起點 | `0x0FD90000`（265,879,552） |
| XDVDFS 簽章 | `MICROSOFT*XBOX*MEDIA`，位於分割區起點 + `0x10000` |
| 根目錄 | 相對 sector 1,783,929；14,336 bytes；讀得 628 entries |
| default.xex | 相對 sector 1,776,975；14,241,792 bytes；`XEX2` |
| XEX SHA-256 | `3d58636bec9b92948f7dbe423c0aa96753349ba014194c4e62d2f809c14643a7` |
| Title ID | `0x5345084D` |
| XEX version 原始欄位 | `0x00000003`，不自行推斷更新包版本 |
| Entry point | `0x824D22F0` |
| Image base | `0x82000000` |
| PE data offset | `0x5000` |
| File format | encryption 1；compression 1（依 Xenia 定義為 normal encryption / basic compression） |
| Original PE name | `SRN.exe` |
| 匯入標頭可讀名稱 | `xam.xex`、`xboxkrnl.exe`；尚未解出逐項 import ordinal |

根目錄有 `shader`、`sound`、`movie`、`$SystemUpdate`，亦有 `NuiIdentity.bin.be` 與 `nuisp*` 檔案。XEX 靜態函式庫清單包含 `NUI`、`NUISP`、`D3D9`、`D3DX9`、`XGRAPHC`、`XAUDIO2`、`XAVATAR`、`XONLINE` 等；出現在清單中不代表各函式都會在遊戲路徑上使用。不能將 `$SystemUpdate` 當成遊戲 Title Update。

目前工作區沒有 Git repository、原始碼或建置設定，也未找到適用的 AGENTS.md。已找到 Git、CMake 4.2.3、Ninja 1.13.2、Visual Studio Community 2022 17.9.4、獨立 LLVM 21.1.8、Android SDK、NDK 29.0.13599879（Clang 20）、Java 21。LLVM 不在目前 PATH，應使用明確路徑或建置環境設定。`VCPKG_ROOT` 未設定。以上為工具存在性／版本檢查，尚未驗證 C++ SDK 連結、Android 建置、Apple 或 Linux 建置環境。

## 參考專案與方案比較

1. **建議：共用工具鏈，逐項移植可重用的執行環境。** XenonRecomp / XenonAnalyse 處理 CPU，XenosRecomp 處理 shader；參考兩個遊戲專案的記憶體、執行緒、檔案、渲染與資產安裝模組。Free Riders 的位址、patch、資產格式與遊戲輸入另行分析。可重用已驗證的設計，同时維持遊戲專用邊界。
2. **直接 fork Marathon 再替換遊戲。** 可較快取得桌面外殼，但其遊戲 patch、shader 資料格式和函式位址都與 Sonic 2006 綁定，需要系統性清除及重做。不能靠修改專案名稱得到 Free Riders。
3. **從零實作全部執行環境。** 邊界容易掌握，但系統服務、音訊與 GPU 工作量最大；只有在上游模組無法合理分離時才局部採用。

Marathon 的主專案公開描述支援 Windows/Linux/macOS；其建置檔包含 Apple 圖形與平台分支。Unleashed 的建置架構同樣將工具、遊戲重編譯程式庫與應用程式分開。這些都是參考能力，不是 Free Riders 已具備的支援。

XenonRecomp 明確表示只轉換 CPU 程式，不提供 runtime。函式邊界、jump table、暫存器保存／還原函式和間接呼叫需要依本遊戲驗證。XenosRecomp 的 shader 翻譯也不能取代資產解包及 GPU 狀態適配。

## 建議架構

資料流：使用者 ISO → 可重現擷取與版本核對 → XEX 載入／分析 → 遊戲專用設定 → 生成 C++ → 目標平台 AOT 建置。shader 從已辨識的資產讀出，經轉換與平台編譯後交由渲染執行環境使用。

- **資產工具**：唯讀 ISO 掃描、受限目錄擷取、manifest、雜湊與版本核對。拒絕越界 entry、目錄循環、路徑穿越及不支援版本；失敗時不留下可被誤認為完成的安裝。使用獨立輸出目錄，保留來源 ISO。
- **CPU 分析與產生器**：固定上游 commit，使用實際 `default.xex` 分析；記錄未解析分支、未支援指令、函式邊界與產生器版本。未確認的位址不得抄用其他遊戲設定或填入假值。先保留保守語意，再以證據啟用最佳化。
- **主機執行環境**：guest 記憶體、位元序、imports、TLS／執行緒、計時、同步、檔案與存檔。未實作的關鍵 import 應清楚中止並記錄呼叫資訊；不得用一律成功的 stub 掩蓋功能缺失。
- **遊戲適配層**：所有 Free Riders 函式位址、hook、資產格式與版本條件集中管理。辨識體感校正與動作消費位置，在適當的遊戲動作層接入統一輸入；必要時補足最低限度 NUI 狀態，以實際流程決定。
- **圖形／音訊**：辨識 shader 容器、資源上傳、材質、繪圖狀態與 render target。評估重用上游圖形抽象；Windows/Linux/Android 以 Vulkan 為候選，Apple 以 Metal 路徑為候選，均須先做能力驗證。音訊從實際資產與呼叫確認格式，重用適合的混音／解碼模組。
- **輸入與平台外殼**：以 SDL 為候選共用輸入／視窗層；平台分開實作資產選取、存檔路徑、生命週期與封裝。手機需正確處理觸控、陀螺儀校正、背景恢復及音訊中斷。

專案原始碼、來源 ROM、擷取資產、生成碼與建置輸出分開存放。建立 Git 時先設定排除來源 ROM 和私人資產；不將其上傳至遠端或 CI。若採用上游 GPL 程式碼，保留其授權及來源紀錄，逐項整理依賴授權。

## 分階段實作與驗收

| 階段 | 交付 | 通過條件 |
| --- | --- | --- |
| M0：來源與工具鏈 | 可重跑的 ROM／XEX 清單工具、擷取工具、manifest、固定依賴版本及建置環境 | 本 ISO 重現上述 XEX 指紋；錯誤輸入與越界／惡意路徑會被拒絕；工具能在已設定 Windows 環境建置 |
| M1：CPU 靜態重編譯 | 遊戲專用分析設定、生成碼、imports 清單及最小診斷 runtime | Windows 執行實際重編譯入口，產生可追蹤的首批系統呼叫；未支援分支／指令明確列出，不能僅以生成成功視為可玩 |
| M2：遊戲啟動 | 檔案、執行緒、shader、圖形、音訊及選單輸入 | 真正到達遊戲標題與選單；可用手把／鍵盤前進，無永久 Kinect 等待 |
| M3：完整單場流程 | 移動、技巧、道具、HUD、暫停與存檔 | 可選角、載入賽道、完成一場比賽、返回選單，存檔可重載；對照原作檢查音畫及遊戲規則 |
| M4：Linux/macOS | 桌面平台建置與封裝 | 兩平台各自重跑 M3；macOS 至少驗證 Apple Silicon ARM64，Linux 至少 x86-64 |
| M5：Android/iOS | ARM64 native 外殼、觸控／陀螺儀、生命週期 | 實機各自重跑 M3，並驗證背景切換、控制器連線變化、存檔和記憶體峰值 |
| M6：發布品質 | 遊戲模式／賽道回歸、效能調整、使用說明與各平台套件 | 列明已驗證的內容及缺陷；測試各平台實際套件，不以 CI 綠燈代表遊玩完成 |

Windows 為首個驗證平台，五平台仍是整體目標。M0 到 M3 是依序降低核心未知的路徑，後續平台共用已驗證的遊戲核心。Android/iOS 不是簡單增加 CMake preset：需另測 ARM64 語意、記憶體配置、圖形能力與平台生命週期。

## 未解問題與處理方式

尚未確認 CPU 函式數量、全部 imports、shader 格式、遊戲更新需求、例外處理、Kinect 內部狀態機或五平台效能。M0/M1 必須產出實測清單，之後依結果調整工作量，現在不承諾完成日期或最低硬體規格。

目前沒有本輪可用的 Linux／Mac／iOS 執行驗證證據。Apple 平台後續需 macOS/Xcode 與測試裝置；iOS 封裝及安裝需配置適當簽署。這些需求在進入相應階段時處理，不能把 Windows 上的交叉編譯宣稱為 Apple 實機驗證。

第一個實作工作包是 M0 與 M1：建立可重現的來源分析與工具鏈，執行真正的重編譯入口並暴露需要實作的 imports。完成這個工作包仍不等於已完成遊戲移植。

## 來源

2026-09-09 讀取；本輪參考的是上游 main，實作時另固定 commit。

- Marathon Recompiled：https://github.com/sonicnext-dev/MarathonRecomp
- Marathon 建置：https://github.com/sonicnext-dev/MarathonRecomp/blob/main/docs/BUILDING.md
- Marathon 平台與模組：https://github.com/sonicnext-dev/MarathonRecomp/blob/main/MarathonRecomp/CMakeLists.txt
- Unleashed Recompiled：https://github.com/hedge-dev/UnleashedRecomp
- Unleashed 建置結構：https://github.com/hedge-dev/UnleashedRecomp/blob/main/CMakeLists.txt
- XenonRecomp：https://github.com/hedge-dev/XenonRecomp
- XenosRecomp：https://github.com/hedge-dev/XenosRecomp
- XEX 欄位定義：https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/util/xex2_info.h
