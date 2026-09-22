# Vulkan 後端

`SFR_GRAPHICS=vulkan` 讓遊戲改用 Vulkan（預設仍是 Direct3D 12）；啟動器的「進階」分類也可以選。
兩個後端共用同一套繪製流程（`native_renderer.cpp`），差別只在以下幾處。

## 裝置與顯示

- `NativeGraphics` 依 `SFR_GRAPHICS` 建立 Plume 的 D3D12 或 Vulkan 介面，整個程序只選一次
  （著色器快取依它準備 DXIL 或 SPIR-V）。
- Plume 的 Vulkan 交換鏈在第一次 `resize()` 才建立影像，所以顯示端建構時就呼叫一次；
  取得影像與呈現用 semaphore 串起來（D3D12 不需要）。Vulkan 版預設開啟垂直同步，這裡改成和
  D3D12 一樣預設關閉（`SFR_VSYNC=1` 開啟）。
- Plume 沒有「貼圖複製到緩衝區」，擷取畫面（`SFR_SCREENSHOT`）在 Vulkan 上直接呼叫
  `vkCmdCopyImageToBuffer`。
- 縮放到視窗用的 blit 著色器（`src/shaders/blit.hlsl`）在建置時由 dxc-bin 編成 DXIL 與 SPIR-V
  並嵌入程式，不再於執行時載入 Windows SDK 的編譯器。

## 著色器

XenosRecomp 翻譯出的 HLSL 本來就有 SPIR-V 分支：常數透過 push constants 裡的三個緩衝區位址
（頂點、像素、共用常數）以 `vk::RawBufferLoad` 讀取，貼圖與取樣器沿用 descriptor set 0–4。

本專案自己加進著色器的部分在 SPIR-V 上另外處理（`src/vulkan_shader_source.cpp`）：

| 內容 | D3D12 | Vulkan |
| --- | --- | --- |
| 螢幕座標換算 `g_ScreenSpaceScale` | 共用常數 `c20.xy` | 共用常數位元組 320 |
| 骨架調色盤 | root CBV b3 | 位址放在共用常數位元組 328 |
| 迴圈常數 i0–i15 | root CBV b4 | 位址放在共用常數位元組 336 |

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
