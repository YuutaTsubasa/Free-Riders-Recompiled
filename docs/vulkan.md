# Vulkan 後端

`SFR_GRAPHICS=vulkan` 讓遊戲改用 Vulkan（預設仍是 Direct3D 12）；啟動器的「進階」分類也可以選。
兩個後端共用同一套繪製流程（`native_renderer.cpp`），差別只在以下幾處。

## 裝置與顯示

- `NativeGraphics` 依 `SFR_GRAPHICS` 建立 Plume 的 D3D12 或 Vulkan 介面，整個程序只選一次
  （著色器快取依它準備 DXIL 或 SPIR-V）。
- Plume 的 D3D12 後端用到 `ID3D12GraphicsCommandList7`（enhanced barriers），只有新的 D3D12
  執行環境才有：Windows 11 24H2 內建，Windows 10 沒有（能建立裝置，建立 command list 時回
  `E_NOINTERFACE` 0x80004002）。所以 Windows 版帶 D3D12 Agility SDK 的 `D3D12Core.dll`：
  `scripts/fetch_d3d12_agility.py`（Windows 上由 bootstrap 執行）依 SHA-256 取得固定版本放進
  `tools/d3d12-agility/`，CMake 把它複製到程式旁的 `D3D12\`，並由 `src/d3d12_agility.cpp`
  從程式本身匯出該版本號（`D3D12SDKVersion`）與路徑。Plume 自己也無條件匯出這兩個符號，
  但版本是建置用 Windows SDK 的，與帶的執行環境不一定相同，所以 CMake 在編譯時把 Plume 的
  改名。
- 選 D3D12 時會先照 Plume 的條件逐一測試顯示卡（能建立 `ID3D12Device8`、回答 Shader Model
  查詢、建立 `ID3D12GraphicsCommandList7`）；一張都不行就自動改用 Vulkan。`game.log` 會為每張
  卡記一行 `NATIVE_GRAPHICS_D3D12_PROBE`（失敗的 HRESULT），改用時記
  `NATIVE_GRAPHICS_FALLBACK`。Windows 10 內建的執行環境也不認得 `D3D12_OPTIONS13`，查不到就
  當作不支援反轉的 viewport 深度。
- Plume 的 Vulkan 交換鏈在第一次 `resize()` 才建立影像，所以顯示端建構時就呼叫一次；
  取得影像與呈現用 semaphore 串起來（D3D12 不需要）。Vulkan 版預設開啟垂直同步，這裡改成和
  D3D12 一樣預設關閉（`SFR_VSYNC=1` 開啟）。
- Plume 沒有「貼圖複製到緩衝區」，擷取畫面（`SFR_SCREENSHOT`）在 Vulkan 上直接呼叫
  `vkCmdCopyImageToBuffer`。
- 縮放到視窗用的 blit 著色器（`src/shaders/blit.hlsl`）在建置時由 dxc-bin 編成 DXIL 與 SPIR-V
  並嵌入程式，不再於執行時載入 Windows SDK 的編譯器。

## 著色器

XenosRecomp 翻譯出的 HLSL 本來就有 SPIR-V 分支：常數透過 push constants 裡的緩衝區位址
（頂點、像素、共用常數）以 `vk::RawBufferLoad` 讀取。本專案把這些讀取改成 uniform buffer
（`src/vulkan_shader_source.cpp`，著色器 ABI 11）：透過位址讀取的常數無法被 Adreno 編譯器移進
每次繪製只跑一次的 preamble，AYN Thor 上的頂點著色器因此每個頂點要做約 130 次記憶體讀取。

| Set | 內容 |
| --- | --- |
| 0 | 貼圖（`Texture2D`、`Texture2DArray`、`TextureCube` 三種型別共用 binding 0） |
| 1 | 取樣器 |
| 2 | 五個 dynamic uniform buffer：b0 頂點常數 4 KB、b1 像素常數 4 KB、b2 共用常數 512 B、b3 骨架調色盤 16 KB、b4 迴圈常數 256 B |

Set 2 每次繪製以 dynamic offset 指向上傳環（`native_renderer.cpp`），不需要每次繪製取緩衝區位址。
只有三個 set，在 Mali-G57 的上限四個之內（Issue #1）。Push constants 仍宣告（位址改成 `uint2`，
避免要求 `shaderInt64`）但不再被讀取。

### 沒有 descriptor indexing 的驅動程式

Set 0 與 set 1 是只填了一部分的 4096 張貼圖與 256 個取樣器陣列，需要 descriptor indexing
（runtime descriptor array、partially bound、variable count、update after bind）。Mesa 的 hasvk
（Broadwell HD 5500，Issue #134：每個 stage 128 張貼圖）沒有這些功能，編譯第一條管線時就當掉。
這種驅動程式改走有界佈局（`NativeGraphics::bounded_descriptors`，`game.log` 記
`NATIVE_DESCRIPTORS mode=bounded`）：

| Set | 內容 |
| --- | --- |
| 0 | binding 0：16 張 2D、binding 1：16 張 2D array、binding 2：16 張 cube |
| 1 | 16 個取樣器 |
| 2 | 不變 |

- 每次繪製依共用常數裡 16 個 fetch constant 的貼圖與取樣器索引取得（或建立）一對 set，第 s 個
  元素就是 fetch constant s 的貼圖，共用常數改寫成 0–15 再寫進上傳環（2D 索引的最高位，即解析
  過的畫面，保留）。沒用到的元素放讀起來是 0 的同型別貼圖，因為沒有 partially bound 時每個元素
  都必須有效。set 來自所屬上傳環的 pool，依內容保留到下一個畫格（`src/bounded_descriptor_cache.h`）：
  貼圖 set 以每個元素的 2D 貼圖索引為鍵（沒有 view 的記成 `null_2d`；緊湊佈局以不同貼圖的清單
  為鍵），只要這些索引的 view 沒換過就繼續用（以時間戳記判斷，因為驅動程式可能把已銷毀 view 的
  handle 給新的 view）；取樣器 set 以取樣器索引為鍵。set 寫好後不再改寫：貼圖換過就在 pool 裡
  丟下舊的、另建一個。環被重用（GPU 已完成）時，丟下的超過 256 個或保留的超過 2048 個才整個
  pool 重設。只重新綁定有變的那個 set。比賽中每畫格約 2 個新 set、約 100 個 descriptor 寫入
  （原本每畫格重建約 270 個 set、寫 12,600 個）；`SFR_FRAME_METRICS=1` 的 `NATIVE_PRESENT` 記
  `descriptor_sets`、`descriptor_writes`、`descriptor_binds` 與 `descriptor_ms`。
- 著色器包不變：建立著色器時改寫 SPIR-V（`src/spirv_bounded_descriptors.h`）。set 0/1 的
  runtime array 改成長度 16 的陣列，三種貼圖依型別移到 binding 0/1/2，拿掉
  `RuntimeDescriptorArray`、`ShaderNonUniform`、`SPV_EXT_descriptor_indexing` 與 `NonUniform`，
  改宣告 `SampledImageArrayDynamicIndexing`（Vulkan 1.0 的 `shaderSampledImageArrayDynamicIndexing`；
  索引來自共用常數，對整次繪製一致）。不認得的模組（其他 set 的陣列、3D 貼圖陣列、其他
  non-uniform 能力）直接停止並說明原因。
- 有界佈局本身需要 `shaderSampledImageArrayDynamicIndexing`、每個 stage 48 張貼圖、16 個取樣器、
  5 個 dynamic uniform buffer、合計 70 個資源與 3 個 set；連這些都沒有的驅動程式仍會停止，並列出
  缺少的項目。
- 舊的 Mali 驅動程式（Galaxy S9 的 Mali-G72，r19：每個 stage 只有 16 張貼圖）放不下 48 張，改用
  緊湊佈局（`BoundedLengths`，`NATIVE_DESCRIPTORS ... compact=1`）：binding 0 是 14 張 2D，binding
  1、2 各 1 張（反正只有 2D 貼圖，另兩種一律讀到 0）。元素 0 讀到 0，其後依序放這次繪製用到的不同
  2D 貼圖，共用常數的 2D 索引改寫成元素編號；超過 13 張時其餘讀到 0，並記一次
  `NATIVE_DESCRIPTORS bounded_overflow`（實際比賽沒有出現）。每個 stage 少於 10 張就停止。
  `SFR_TEST_BOUNDED_TEXTURES=<n>` 把每個 stage 的貼圖上限當成 n，在其他顯示卡上測試這個佈局。
- `SFR_TEST_NO_DESCRIPTOR_INDEXING=1` 讓 Plume 建立裝置時不開 descriptor indexing 功能
  （`patches/plume-descriptor-indexing-test-switch.patch`），在有這些功能的顯示卡上也走有界佈局；
  搭配 `-DSFR_VULKAN_VALIDATION=ON` 與驗證層，可檢查是否還用到裝置沒開的功能。
- 有 descriptor indexing 的 Vulkan 與 D3D12 不受影響。

### Vulkan 1.1 的驅動程式

Plume 原本把 `VK_EXT_scalar_block_layout` 與 `VK_KHR_buffer_device_address` 列為必要擴充功能，
沒有的驅動程式（Huawei P30 Pro 的 Mali-G76、Android 10、Vulkan 1.1，Issue #169）連裝置都建立不了。
實際上兩者都用不到：

- 著色器包的 476 個 SPIR-V 模組沒有一個宣告 `PhysicalStorageBufferAddresses`（常數已改由 set 2
  的 uniform buffer 讀取），`spirv-val` 不加 `--scalar-block-layout` 也全部通過（`-fvk-use-dx-layout`
  的偏移量符合 Vulkan 1.1 的 relaxed block layout）。渲染器也不再取緩衝區位址。
- `patches/plume-optional-scalar-layout-device-address.patch` 把兩者改成選用：有就照舊開啟，沒有時
  capabilities 回報 `scalarBlockLayout=0`、`bufferDeviceAddress=0`，緩衝區不加
  `SHADER_DEVICE_ADDRESS` 用途。

但 DXC 輸出的是 SPIR-V 1.5，需要 Vulkan 1.2。實例與裝置版本中較低者小於 1.2 時，建立著色器前把
模組改成 SPIR-V 1.3（`src/spirv_version.h`）：版本號改為 1.3，進入點介面只留 Input/Output 變數
（1.4 起介面列出所有用到的全域變數）。遇到 1.3 沒有的東西（1.4/1.5 指令、組合型別的
`OpSelect`、Vulkan 記憶體模型、physical storage buffer）直接停止並說明原因。整個著色器包（原樣，
以及加上拿掉 clip distance 與有界佈局兩種改寫之後）改成 1.3 後，`spirv-val --target-env vulkan1.1`
全部通過。`game.log` 記 `NATIVE_SPIRV api=… version=1.3|1.5 scalar_block_layout=… device_address=…`。

測試開關（只在 Vulkan 生效）：

- `SFR_TEST_VULKAN_1_1=1`：Vulkan 實例只要求 1.1，驗證層就以 1.1 的規則檢查裝置與著色器，
  著色器也改成 SPIR-V 1.3。
- `SFR_TEST_NO_SCALAR_LAYOUT=1`、`SFR_TEST_NO_DEVICE_ADDRESS=1`：建立裝置時不開這兩個功能與擴充。
- `SFR_TEST_DEPTH_D24S8=1`：深度格式改用 D24S8（見下）。
- 以上加上 `SFR_TEST_NO_DESCRIPTOR_INDEXING=1`、`SFR_TEST_NO_CLIP_DISTANCE=1` 與
  `SFR_TEST_BOUNDED_TEXTURES=16`，大致就是舊 Mali 的情況。

### 深度格式

所有深度目標（畫面、render surface、解析出的深度）與繪製管線都用 `NativeGraphics::depth_format`：
optimal tiling 能當 depth-stencil attachment 又能取樣的話用 `D32_SFLOAT_S8_UINT`，否則用
`D24_UNORM_S8_UINT`（`patches/plume-d24s8-depth.patch` 在 Plume 的 `RenderFormat` 最後加上它），
兩者都沒有就停止並說明原因。`game.log` 記 `NATIVE_DEPTH format=D32S8|D24S8 d32s8=… d24s8=… forced=…`。

Firebase Test Lab 的 Galaxy S9（Mali-G72，Android 10，驅動程式 r19，Vulkan 1.1.108）回報
`scalar_block_layout=1 device_address=0`：改版前一樣會停在建立裝置。它也沒有 `D32_SFLOAT_S8_UINT`
（只有 D24S8），因此需要上面的深度格式。

### 頂點屬性數

每個繪製都為 XenosRecomp 頂點著色器可能讀的 20 個輸入位置各宣告一個頂點屬性
（`native_vertex_layout.cpp`，宣告裡沒有的讀 slot 15 的零）。Vulkan 只保證 16 個
（`maxVertexInputAttributes`，位置也要小於它），舊 Mali 正好是 16：v0.10.3 在 Huawei P30 Pro
（Mali-G76，驅動程式 r18）上第一個繪製就讓 GPU 停住，`vkWaitForFences` 回 `VK_ERROR_DEVICE_LOST`，
整支手機卡住（Issue #169）。Firebase Test Lab 的 Galaxy S9（Mali-G72，r19）用圖形自我測試的繪製
重現了同樣的停住：遊戲的頂點著色器配 20 個屬性就停在第一個繪製；換成不讀輸入的頂點著色器，
或遊戲的像素著色器配簡單的頂點著色器，455 條管線都畫得完。桌面驗證層看不到，因為桌面驅動程式
有 28 個以上；用 profiles 層把上限報成 16 時，驗證層對每條管線都報
`VUID-VkPipelineVertexInputStateCreateInfo-vertexAttributeDescriptionCount-00614` 與
`VUID-VkVertexInputAttributeDescription-location-00620`。

屬性不到 20 個的裝置上（`NativeGraphics::compacts_vertex_inputs`），每個頂點著色器的輸入依原本
位置的順序改到 0、1、2…（`spirv_vertex_inputs.h`，原位置記在 `VertexInputUse`），它的管線只宣告
它讀的那些屬性，搬到新位置。包裡 287 個頂點著色器最多讀 14 個輸入，133 個讀到 15 以上的位置。
屬性夠的裝置不變。`game.log` 記 `NATIVE_VERTEX_ATTRIBUTES limit=… forced=… compacted=0|1`；
`SFR_TEST_VERTEX_ATTRIBUTES=<n>` 把裝置的屬性數當成 n（16 在任何顯示卡上試這條路，32 在舊 Mali
上關掉它）。

### Apple：拿掉分派迴圈

客機著色器有跳躍或迴圈時，XenosRecomp 寫成 `uint pc = 0; while (true) switch (pc) {...}`：每個
case 落到下一個，或設 pc 後 continue。DXC 把它變成一個迴圈，標頭用 phi 帶著所有暫存器進一個對
pc 的 switch。Apple 的 Metal 編譯器（MoltenVK，macOS 與 iOS）在這種形狀上每個執行緒溢出 6–8 KB，
蒙皮（palette）頂點著色器最多：M1 上一場比賽每幀 ~145 ms 都在頂點階段。

Apple 上 `NativeGraphics::shader_code` 先把它拿掉（`spirv_dispatch_loop.h`）：case 照順序排，每個
前面一個只對自己 pc 的 switch，出口設下一個 pc（下一個 case、continue 指定的、break 就是沒有）後
跳到那個 switch 的 merge；只有往回的 continue 跨過的 case 外面包一個迴圈。原本迴圈帶著的值和所有
在別的 block 用到的值放 Function 變數（Metal 會放回暫存器）。每條原指令執行順序不變，NoContraction
也留著。包裡 476 個著色器中 242 個頂點著色器有分派迴圈，全部改得了、`spirv-val` 都過；單元測試用
一個小 SPIR-V 直譯器確認改前改後輸出逐位元相同（`SFR_SHADER_PACK=<pack>` 時跑整包）。

M1（Metal System Trace，比賽中 8 秒）：頂點 144.6 → 6.4 ms/幀，GPU 6.5 → 60 fps，最大溢出 6992 →
608 bytes，`RACE_FRAMESKIP speed` 0.148 → 0.997。Metal 上 gl_Position 逐位元相同；部分 texcoord
差幾十 ULP（編譯器對非 precise 運算的合併不同），畫面像素差最多 1–2 階。`game.log` 記
`NATIVE_DISPATCH_LOOP removed=0|1`；`SFR_TEST_DISPATCH_LOOP=0` 在 Apple 上保留迴圈，`=1` 在任何
Vulkan 裝置上拿掉（驗證層用）。

Samsung Xclipse（vendor 0x144d，Exynos 手機上以 AMD RDNA 為基礎的 GPU）也一律拿掉（Issue #224）。
Galaxy A55（Xclipse 530，驅動 24.0.539/545）編第一條管線（頂點著色器 `eaf61b182ad92401`）時，
驅動的編譯器（logcat `GSC`）花了 209 秒、常駐記憶體漲到 550 MB 以上，最後在組譯器
`SCGenerateLongBranch` 斷言失敗；玩家看到「PREPARING PIPELINES 0 OF 455」直到 Android 因記憶體
不足把程式收掉。拿掉迴圈後 455 條 14 秒編完（最慢的一條 253 ms）。描述符索引、device address、
clip distance 的測試開關都沒用。Firebase Test Lab 的 a55xzh（Galaxy A55）可以重現。

Intel GPU（vendor 0x8086）也一律拿掉。有迴圈時一格比賽的 GPU 時間是 115–185 ms，而且跟解析度無關
（Issue #134，HD 5500）；拿掉後在 UHD 620（Linux、Mesa 26.2）上是 20–36 ms，比賽從約 7 fps 變成全速
（略過繪製補上）（Issue #239）。Intel 只走 Vulkan 時才有影響（Windows 預設的 D3D12 不受影響）。

驅動可能拒絕拿掉迴圈後的管線。AYN Thor（Adreno 740，驅動 512.676.53）用 `SFR_TEST_DISPATCH_LOOP=1`
時，455 條裡有 17 條 `vkCreateGraphicsPipelines` 回 `VK_ERROR_UNKNOWN`（logcat `AdrenoVK-0: Failed to
link shaders.`），全都用到同樣 11 個大的頂點著色器；比賽開始時遊戲停在 `native graphics pipeline
creation failed`。改寫後的 SPIR-V 都過 `spirv-val`，問題在 Adreno 的編譯器處理不了那麼多 Function
變數：每個變數先給初值，11 個裡有 9 個就連得起來；用 `spirv-opt --ssa-rewrite` 把變數變回 SSA
（phi）則 17 條全部連得起來。現在一條管線用拿掉迴圈的著色器做不出來時，就用保留迴圈的同一個
著色器再做一次（`NativeGraphics::with_dispatch_loop`，每個著色器第一次需要時才做），`game.log`
每條記一行 `NATIVE_DISPATCH_LOOP_FALLBACK recipe=<n>|vs=<hash> ps=<hash> vertex=0|1 pixel=0|1
made=0|1`。管線快取（`VkPipelineCache`）以著色器內容為鍵，學到的配方以原始著色器為準，兩種
都不必另外區分。

Adreno 不預設拿掉迴圈：Thor 上 Dolphin Resort 的比賽（720p，`SFR_GPU_TIMING=1`）保留迴圈時 GPU
每幀約 10 ms，拿掉後約 32–34 ms（SSA 形式也一樣），跳過的繪製從 ~35 變成 ~5000 幀（共 9600）。
Pixel 10a（Mali-G715）也沒有變快（Issue #176 回報者：15–24 ms 對 15–23 ms），Mali 同樣不預設。

準備管線時，一條配方（recipe）超過 5 秒就記 `PIPELINE_PREWARM slow recipe=<n> of=<total>
step=shaders|pipeline vs=<hash> ps=<hash> seconds=<s> resident_mb=<MB>`（之後 10、20、40 秒…再記），
每 10 秒記一次 `PIPELINE_PREWARM progress=`，完成時記最慢的一條；Android 上也寫進 logcat。
`SFR_PIPELINE_PREWARM_TRACE=1` 記每條的時間，`SFR_PIPELINE_PREWARM_TIMEOUT=<s>` 讓一條超過 s 秒時
結束程式（自我測試用，好讓下一個情境能跑）。自我測試的情境 9–14 只準備管線並開著這兩個：
9 照裝置、10 拿掉分派迴圈、11 小型材質組、12 不用 device address、13 不用 clip distance、14 保留
分派迴圈。

### macOS：Retina 與縮放顯示器上的 present

v0.10.14 以前，macOS 的遊戲視窗不是高 DPI：SDL 的 Metal layer 的 contents scale 是 1，1280x720
的視窗 layer 就是 1280x720 像素；Plume 的 `CocoaWindow` 卻用視窗的 backing scale（Retina 或縮放
顯示器上是 2）算 swap chain，做成 2560x1440。MoltenVK 只認 layer 的自然大小（bounds 乘
contents scale），大小不同的 swap chain 每次 present 都回 `VK_SUBOPTIMAL_KHR`，Plume 在 Apple 上把它
當失敗，重建八次後遊戲停在 `failed to present native swap-chain texture`（Issue #191，M1 MacBook
Pro 的內建螢幕和縮放成 2560x1440 的外接螢幕都會，1x 的 1920x1080 螢幕不會）。

- macOS 的遊戲視窗改成高 DPI（`native_graphics.cpp`，和 iOS 一樣），layer 的 scale 就是螢幕的。
- `patches/plume-present-result-layer-size.patch`：macOS 的 swap chain 大小改取視窗裡 Metal layer
  的 bounds 乘 contents scale（截斷，和 MoltenVK 一樣），兩者不會再不同；swap chain 也記下最後一次
  present 與 acquire 的 `VkResult`。
- `game.log` 的 `NATIVE_PRESENTATION present_refused` 與 `acquire_refused` 都記
  `result=<名稱>(<數字>)`（所有平台）；重建八次仍是 `VK_SUBOPTIMAL_KHR` 時記一次
  `NATIVE_PRESENTATION suboptimal_kept`，繼續執行（MoltenVK 仍會把這張圖縮放顯示），不再結束遊戲。

在只有 1x 螢幕的 Mac 上，可以用私有的 `CGVirtualDisplay` 建一個虛擬螢幕，設成 1280x720 HiDPI
（2560x1440 像素）模式來重現。注意：macOS 27 上建立它的程序結束後，虛擬螢幕不會消失（WindowServer
記錄 `CGXVirtualDisplayDestroy ... not found`），要登出或重開機才會移除。

### 圖形自我測試的繪製

`graphics_selftest.cpp`（Android 由 `GraphicsTestActivity` 啟動，桌面設 `SFR_GRAPHICS_SELFTEST=1`
執行 `sfr_cpu_diagnostic`）準備完包裡的管線後，用每條管線畫一次（`NativeRenderer::prepared_draws`，
走遊戲的繪製路徑，頂點與常數是 [-1, 1] 的數，貼圖有一般的、解析出的顏色與深度和空的），每個繪製
單獨送出並等它完成，等了三秒以上就記 `draws hung at=<n>` 並停止。開關：
`SFR_SELFTEST_ORDER=class`（著色器越簡單越先畫：沒有迴圈、switch、取樣的先）、
`SFR_SELFTEST_VS=trivial|few|uniform`、`SFR_SELFTEST_PS=trivial|sample`（換成
`graphics_selftest_shaders.h` 的替身著色器）、`SFR_SELFTEST_NO_ELEMENTS=1`、`SFR_SELFTEST_NO_DEPTH=1`、
`SFR_SELFTEST_TEXTURES=mixed|plain|nodepth|null`、`SFR_SELFTEST_BATCH=<n>`、`SFR_SELFTEST_LIMIT=<n>`、
`SFR_SELFTEST_DRAWS=0`（不畫）。Game loop 的情境 3 到 8 是 `GraphicsTestActivity.DRAW_SCENARIOS`。

本專案自己加進著色器的部分：

| 內容 | D3D12 | Vulkan |
| --- | --- | --- |
| 螢幕座標換算 `g_ScreenSpaceScale` | 共用常數 `c20.xy` | 共用常數位元組 320 |
| 骨架調色盤 | root CBV b3 | set 2 binding 3 |
| 迴圈常數 i0–i15 | root CBV b4 | set 2 binding 4 |

`SharedConstants` 為此從 336 位元組延長到 352。像素著色器的 specialization constant（alpha test）
在 Vulkan 上設定在管線上，不需要 D3D12 那樣用 DXC 連結。模板參考值 Vulkan 也存在管線裡，
不需要 D3D12 的 `OMSetStencilRef` 補丁。

執行期快取（`out/shaders/runtime/v6-*`）在 Vulkan 上從已翻譯的 `shader.hlsl` 產生
`shader.vulkan.hlsl` 與 `shader.spv`，用的是 XenosRecomp 釘選的 dxc-bin（Windows SDK 的 DXC 沒有
SPIR-V 輸出）；DXIL 那一步在 Vulkan 上略過，所以 Vulkan 版執行時不需要 Windows SDK。
SPIR-V 編譯失敗不寫入 `untranslatable.txt`，因為 D3D12 共用同一個資料夾。

## 著色器包

`python scripts/pack_shaders.py` 把執行期快取中已編好的著色器（原始容器、mask、DXIL、SPIR-V）
打包成 `out/shaders/shaders.pack`（`SFR_SHADER_PACK` 可改路徑）。遊戲先查包，查到就不需要翻譯器
或任何 DXC。包裡是從本機遊戲檔取出的著色器，和 `image.bin` 一樣只能留在本機。

限制：遊戲檔中只有 `shader/Xbox360BasicShader.fxobj` 的 68 個著色器是未壓縮的，其餘（目前
遇到的約 400 個）在遊戲執行時才從壓縮檔解出，所以還無法離線一次編好全部；包的內容取決於
已經玩過（跑過）的部分。

## 驗證

```bash
SFR_GRAPHICS=vulkan out/build/host/sfr_native_graphics_test.exe
SFR_GRAPHICS=vulkan out/build/host/sfr_native_presentation_test.exe
```

原生光柵測試直接操作 D3D12，在 Vulkan 上會略過。遊戲本身以 `SFR_GRAPHICS=vulkan` 跑標題、
影片與 Free Race 比對畫面。
