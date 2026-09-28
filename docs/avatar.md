# Avatar / VRM 技術紀錄

這個分支整合 PR #15 的 Avatar body-type 回覆，以及本機 VRM 載入器。
v0.3.0 Preview 提供此功能；使用步驟見 [VRM Avatar guide](vrm-avatar.md) 與
[繁體中文 README](../README.zh-TW.md#vrm-avatar-模型)。目前主要遊玩驗證範圍為 Windows 1P。

## 使用方式

在啟動器設定的 **Advanced／進階** 頁面選擇 **Avatar 模型**，瀏覽本機
`.vrm` 或 `.glb`。路徑會記住，下次啟動遊戲生效；清除即可停用。
手動輸入的相對路徑以啟動器所在資料夾為基準。模型只在本機讀取。

直接啟動 runtime 時，也可以設定以下環境變數（Windows 範例）：

```bat
set "SFR_AVATAR=1"
set "SFR_AVATAR_MODEL=C:\Models\player.vrm"
sfr_cpu_diagnostic.exe "C:\Game\image" "C:\Game\assets" --game-region=ntsc-us
```

在角色選單選 **AVATAR**，再選 Gear 並進入比賽。共用模型設定會套用至選擇 Avatar
的本機玩家，顯示於 Loading 與比賽；一般角色與角色選單不會畫上此模型。
模型讀取失敗會記錄在 `game.log`；模型不會上傳。

- `SFR_AVATAR_MODEL_SCALE`：模型尺寸倍率，預設 `1`。模型以公尺讀取，
  腳底最低點對齊遊戲角色的原點；不同模型仍需要實測尺寸。
- `SFR_AVATAR_MODEL_POSE=rest`：使用檔案原本姿勢；預設依 VRM 人形骨架
  套用遊戲的 Avatar 骨骼動畫；無有效動作時使用中立骨架，避免沿用另一位玩家的姿勢。
- `SFR_AVATAR_BODY=2`：使用女性 Avatar 的遊戲內 body/gear variant。
- `SFR_AVATAR_TEXTURE_MAX`：貼圖最大邊長，預設 `2048`。
- `SFR_AVATAR_MODEL_PLAIN=1`：停用模型貼圖，方便診斷形狀。
- `SFR_TRACE_AVATAR=1`：記錄角色狀態與前幾次矩陣；不記錄攝影機影像。
- `SFR_TRACE_AVATAR_POSE=1`：額外取樣遊戲的骨骼旋轉數值，供動作診斷。

沒有指定模型時，原生 VRM 渲染器不會建立 GPU 資源。
只設定 `SFR_AVATAR=1` 可以測試沒有原始 Xbox Avatar 資產的載入流程，
但不會憑空產生角色模型。

## 本機玩家身分與生命週期

`src/avatar_state.h` 每次讀取目前的 race manager，不缓存上一場的角色指標。

| 狀態 | 遊戲資料 |
| --- | --- |
| 比賽已建立 | `[0x83E52F8C] != 0` |
| 確認後的本機玩家數 | byte `[0x82B0569F]` 為 1 或 2 |
| Race manager | `M = [0x83E52FDC]` |
| Racer vector | begin=`[M+36]`, end=`[M+40]`, count=`[M+20]` |
| 本機 racer | 掃描已初始化 vector 的前 local_count 筆 |
| Avatar 角色 | `[P+100] == 17` |

18 是不同的角色值；17/18 的 body variant 存在 gear/model 欄位，不能一起
當成 Avatar 角色。`XamAvatarManifestGetBodyType` 在選單預覽也會被呼叫，
所以不能把呼叫過一次當成玩家已選 Avatar。`0x83E515FB` 則是雙人選單狀態，
離開選單會清除，也不能代替比賽的玩家數。

所有指標先檢查可讀範圍。Loading 尚未建立 racer、切換角色或 race manager
被銷毀時都停止繪製。Manager `+20` 是預計參賽人數，Loading 的 vector 可能
先只有本機玩家；不要求所有參賽者都載入後才顯示已建立的本機玩家模型。

## 模型位置與相機

舊實驗將模型固定放在相機前方；這個分支已改用遊戲實際的 Avatar 繪製資料。
`sub_823B97A8` 的主畫面呼叫（LR `0x822AB04C`）帶入：

- `r3`：Avatar renderer，必須等於對應本機 racer 的 `[[P+3208]+8]`。
- `r5` / `r6` / `r7`：world / view / projection 矩陣。
- `r9`：camera index，接受已啟用的 0／1，與模型所屬玩家獨立。

矩陣為 row-vector 慣例，位移在元素 12–14；組合為 `local * W * V * P`。
在函式入口複製矩陣，避免保留 stack 指標。陰影 pass（LR `0x82280D8C`）
不繪製自訂模型。直接在這次角色繪製中使用矩陣與當下 viewport／scissor；
跳過繪製的格不會送出自訂繪圖，不保存矩陣到下一格。

同一個呼叫的 `r4` 是動畫物件 B。`[B+11556]` 選擇目前的雙緩衝，
`B+16+buffer*3456` 包含 72 組 translation/quaternion/scale，步長 48 bytes。
在 `sub_823BBCB8` 完成動作片段求值與混合後保存當格結果，核對 controller、
renderer、B 與目前玩家的關聯，繪製與道具只接受同一 manager／玩家／renderer／
controller／B／present 的快照。每位本機玩家保留自己的快照。

這個時間點很重要：後面的 `sub_822A5140` 會依原生 chest 矩陣計算體感肩膀
修正，但缺少 Xbox bind pose 時該矩陣是 identity。直接使用修正後的數值會
重複套入身體轉向、扭曲上半身；VRM 暫時保留修正前的遊戲動作片段，尚未重建
這段逐關節體感修正所需的原生骨架。

已確認的 19 個人形關節驅動 VRM，保留模型的肢體比例及 bind pose。Root 的
平移套用一次，保留蹲下與換邊小跳；其他關節的平移不套用到不同身形。
腳底基準使用固定 bind minimum，不能每格重新貼地而消掉起跳。`byte[renderer+8]`
沿用原版 `sub_823BA028` 的全身 X 鏡射，包含位置、法線與三角形方向，呈現換邊。

每格從保留的原始頂點重算 skinning，更新法線與邊界，不會累積變形。
每次繪製使用獨立頂點緩衝，兩組每格緩衝池輪流使用，避免分割畫面覆写同格資料
或覆寫 GPU 尚在讀取的上一格。這是 CPU skinning，
高面數模型會增加每格 CPU 工作量。

模型在原生角色繪製時進入場景，與場景共用深度。一般 viewport 使用 LESS，
反向深度 viewport 使用 GREATER。之後的 HUD 依遊戲原本順序覆蓋模型。自訂
繪製後會使 NativeRenderer 的 pipeline／descriptor 綁定快取失效，供下次重新綁定。

手持道具的 `8227FD58` 會讀取 `823BC330` 的 Avatar 手部矩陣。僅在這條呼叫
路徑（LR `8227FDA4`）與骨骼 33／36 時，以同格 VRM 手部位置和中和 bind 軸向後
的方向替代空 SDK 矩陣；遊戲繼續負責道具網格、grip offset 與丟出行為。

## 載入與限制

PR #15 補上 body-type 回覆；要實際開始比賽，還需要 Avatar 初始化、manifest、
asset size 與空資產回覆、原始零部件流程的處理，以及 `vcmpbfp128` 指令支援。
原始 Xbox Avatar 的網格不會下載或重建，改由本機模型顯示。

VRM 0.x / 1.0 都以 binary glTF 讀取，使用基本色與基本色貼圖。依每個材質的
`doubleSided` 決定是否畫背面；`KHR_materials_unlit` 保留不受光照影響的顏色，
MToon 材質目前使用它宣告的 unlit fallback。受光材質在 linear colour space
計算光照，避免直接在 sRGB 貼圖上乘亮度造成偏暗。支援 PNG 和
baseline JPEG；不支援 interlaced PNG 或 progressive JPEG，讀不到的貼圖會
退回材質色。讀取器會拒絕循環節點、過深 JSON、截斷 JPEG segment 與超出 PNG
宣告尺寸的解壓資料。

目前仍有限制：只對應主要身體關節，尚無完整手指、表情與 spring bone 動態，
沒有完整 MToon 或透明混色；完整雙人遊玩尚待實測。沒有 VRM 人形骨架對應的普通
GLB 會維持靜態。不同模型的尺寸、骨架與材質需要實際確認。

## 驗證

```powershell
ctest --test-dir out/build/play -R "^(avatar_state|gltf_model|image_decode|vector_compare_bounds)$" --output-on-failure
python -m unittest discover -s tests -p test_diagnostic_generation.py
```

`avatar_state` 測試涵蓋單人角色身分、普通角色、雙人、Loading／離場、無效指標、
矩陣排列與位移、腳底對齊，以及姿勢快照生命週期。GPU 測試涵蓋視窗、深度、
HUD 前後順序與同格多次姿勢繪製。實際遊戲測試另外確認
Avatar 選擇 → Gear → Loading → Free Race，不能只靠模型讀取單元測試判定可玩。
