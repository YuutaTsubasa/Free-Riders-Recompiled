# 1P Avatar / VRM（測試中）

這個分支整合 PR #15 的 Avatar body-type 回覆，以及本機 VRM 載入器。
測試範圍先限於單人比賽；尚未發佈到正式 Release。

## 使用方式

啟動遊戲前設定以下環境變數（Windows 範例）：

```bat
set "SFR_AVATAR=1"
set "SFR_AVATAR_MODEL=C:\Models\player.vrm"
FreeRidersRecompiled.exe
```

在角色選单選 **AVATAR**，再選 Gear 並進入比賽。VRM 只會在目前的單人
Avatar 比賽中顯示；一般角色、角色選單和雙人模式不會畫上這個模型。
模型讀取失敗會記錄在 `game.log`；模型不會上傳。

- `SFR_AVATAR_MODEL_SCALE`：模型尺寸倍率，預設 `1`。模型以公尺讀取，
  腳底最低點對齊遊戲角色的原點；不同模型仍需要實測尺寸。
- `SFR_AVATAR_MODEL_POSE=rest`：使用檔案原本姿勢；預設依 VRM 人形骨架
  烘焙一個固定滑行姿勢。
- `SFR_AVATAR_BODY=2`：使用女性 Avatar 的遊戲內 body/gear variant。
- `SFR_AVATAR_TEXTURE_MAX`：貼圖最大邊長，預設 `2048`。
- `SFR_AVATAR_MODEL_PLAIN=1`：停用模型貼圖，方便診斷形狀。
- `SFR_TRACE_AVATAR=1`：記錄角色狀態與前幾次矩陣；不記錄攝影機影像。

沒有指定模型時，原生 VRM 渲染器不會建立 GPU 資源。
只設定 `SFR_AVATAR=1` 可以測試沒有原始 Xbox Avatar 資產的載入流程，
但不會憑空產生角色模型。

## 1P 身分與生命週期

`src/avatar_state.h` 每次讀取目前的 race manager，不缓存上一場的角色指標。

| 狀態 | 遊戲資料 |
| --- | --- |
| 比賽已建立 | `[0x83E52F8C] != 0` |
| 確認後的本機玩家數 | byte `[0x82B0569F] == 1` |
| Race manager | `M = [0x83E52FDC]` |
| Racer vector | begin=`[M+36]`, end=`[M+40]`, count=`[M+20]` |
| 第一位 racer | `P = [begin]` |
| Avatar 角色 | `[P+100] == 17` |

18 是不同的角色值；17/18 的 body variant 存在 gear/model 欄位，不能一起
當成 Avatar 角色。`XamAvatarManifestGetBodyType` 在選單預覽也會被呼叫，
所以不能把呼叫過一次當成玩家已選 Avatar。`0x83E515FB` 則是雙人選單狀態，
離開選單會清除，也不能代替比賽的玩家數。

所有指標先檢查可讀範圍。Loading 尚未建立 racer、切換角色或 race manager
被銷毀時都停止繪製。

## 模型位置與相機

舊實驗將模型固定放在相機前方；這個分支已改用遊戲實際的 Avatar 繪製資料。
`sub_823B97A8` 的主畫面呼叫（LR `0x822AB04C`）帶入：

- `r3`：Avatar renderer，必須等於第一位 racer 的 `[[P+3208]+8]`。
- `r5` / `r6` / `r7`：world / view / projection 矩陣。
- `r9`：camera index，1P 使用 0。

矩陣為 row-vector 慣例，位移在元素 12–14；組合為 `local * W * V * P`。
在函式入口複製矩陣，避免保留 stack 指標。陰影 pass（LR `0x82280D8C`）
不會覆蓋主畫面的相機。每個 present 都消耗或清除一次資料，包含跳過繪製的格，
不會沿用上一格或上一場的矩陣。

模型仍在遊戲畫完該格後合成，使用自己的深度緩衝；因此模型自身有遮擋，
但場景不會遮住它，而且它可能蓋到 HUD。還沒有完整的場景內渲染。

## 載入與限制

PR #15 補上 body-type 回覆；要實際開始比賽，還需要 Avatar 初始化、manifest、
asset size 與空資產回覆、原始零部件流程的處理，以及 `vcmpbfp128` 指令支援。
原始 Xbox Avatar 的網格不會下載或重建，改由本機模型顯示。

VRM 0.x / 1.0 都以 binary glTF 讀取，使用基本色與基本色貼圖。支援 PNG 和
baseline JPEG；不支援 interlaced PNG 或 progressive JPEG，讀不到的貼圖會
退回材質色。讀取器會拒絕循環節點、過深 JSON、截斷 JPEG segment 與超出 PNG
宣告尺寸的解壓資料。

目前仍有限制：固定滑行姿勢、沒有角色動態動畫、沒有完整 MToon 或透明混色，
也沒有雙人 VRM。不同模型的尺寸、骨架與材質需要實際確認。

## 驗證

```powershell
ctest --test-dir out/build/play -R "^(avatar_state|gltf_model|image_decode|vector_compare_bounds)$" --output-on-failure
python -m unittest discover -s tests -p test_diagnostic_generation.py
```

`avatar_state` 測試涵蓋單人角色身分、普通角色、雙人、Loading／離場、無效指標、
矩陣排列與位移、腳底對齊，以及每格矩陣的消耗。實際遊戲測試另外確認
Avatar 選擇 → Gear → Loading → Free Race，不能只靠模型讀取單元測試判定可玩。
