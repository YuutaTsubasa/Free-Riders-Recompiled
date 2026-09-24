# Vulkan 的 push constant 位址

## 2026-09-24：Adreno 750 的實機定位與修正（快取 v8）

在 AYANEO Pocket S2 Pro、Android 14、Adreno 750 上，將 push constant 位址宣告成
`uint64_t` 的 fragment shader 出現錯誤。將欄位改為 `uint2`，讀出後再組回 64-bit
位址，可以避開這條編譯路徑；CPU 傳送的 5 個位址、40-byte 佈局與 BDA 讀取方式不變。
實測 Vulkan API 為 1.3.128，Qualcomm driverVersion 為 `0x802fa018`，driverInfo：
`Build 43c70540de, Ibee7afe120, 1721201467; Date 07/17/24; Compiler E031.45.02.16`。

`vulkan_shader_source.cpp` 現在為兩個 shader 階段產生：

```hlsl
struct PushConstants {
    uint2 VertexShaderConstants;
    uint2 PixelShaderConstants;
    uint2 SharedConstants;
    uint2 VertexPalette;
    uint2 LoopConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

uint64_t sfrBufferAddress(uint2 address) {
    return uint64_t(address.x) | (uint64_t(address.y) << 32);
}
// 仍是 physical storage buffer load，沒有換成 descriptor。
// vk::RawBufferLoad<float4>(sfrBufferAddress(g_PushConstants.VertexPalette)
//                          + uint64_t(index) * 16, 0x10)
```

定位證據：

- 原始 APK 的 468 個 shaders 先重建成逐位元組相同的基準，再只將 DXC target 改為
  Vulkan 1.2，畫面仍錯。因此 target env 不是這次的充分修正。
- 獨立 compute probe 的 BDA float4、int4、scalar、間接位址與 SSBO 對照，4096 筆
  全部相符。單純「Adreno 不支援 BDA」不符合觀察。
- 從手機擷取 20 組真實 vertex shader 輸入，在 Windows Vulkan 與 Adreno 重播；輸入及
  SV_Position 在相對容差 `1e-4 * max(1, abs(a), abs(b))` 內一致。v8 再重播一次也一致。
  這是有輸出儲存指令的探針，不代表原始 shader 所有編譯路徑都已排除。
- 同一場景只換成純色 pixel shader，完整角色輪廓出現；停用剔除或深度測試無法修正
  原始 pixel shader 的畫面。
- fragment probe 比較 push constant 位址間的固定差值：`uint64_t` 欄位失敗，改成
  `uint2` 後成功。同一個 probe 比較 BDA／SSBO 的 texture 與 sampler index，原先不符，
  改成 `uint2` 後相符。
- 僅修改 pixel shaders 的這個表示方式，就消除了測試場景中角色、頭像與賽道的大片
  碎片及錯色。此結果定位到這版驅動的 64-bit push constant 處理路徑；不是所有 Adreno
  驅動皆有問題的宣稱，也尚未取得驅動廠商確認。

編譯現在明確指定 `-fspv-target-env=vulkan1.2`，並將 runtime cache 和
`scripts/pack_shaders.py` 一起升為 `v8-`。468 個正式轉換輸出的 SPIR-V 均通過
`spirv-val --target-env vulkan1.2 --scalar-block-layout`，來源改寫回歸測試通過。
DXIL 保持原樣。舊 pack 不會自動重寫，必須重新編譯／打包並替換 Android 上的 pack。

本機重現資料放在未追蹤的 `out/adreno-investigation/`：原始 APK／pack、A/B 截圖、
vertex captures、重播工具，以及 `diagnostic-instrumentation.patch`。正式 renderer 已移除
熱換 shader、額外 descriptor、額外 push constant 與逐 draw 檔案檢查等臨時探針。

效能另計：關閉擷取並恢復頂點快取後，pixel-only 修正版連續 100 個比賽幀約 7.3 FPS；
`record_ms` 中位數 5.57 ms，`draw_ms` 11.24 ms，`main_queued_ms` 73.60 ms，
`gpu_wait_ms` 0.48 ms。此測試使用 `SFR_PARALLEL_WORKER=1`（僅 job worker 並行）。
`cores` 模式在這次 Android build 會卡在開場影片，尚未修正；不能把 7.3 FPS 視為
恢復完整並行後的效能，也不能把圖形修正當作效能修正。

移除診斷程式、兩個階段均採 v8 的最終 APK 已重新安裝並進入 Free Race，角色、頭像
與賽道的上述錯誤消失。最後 100 個比賽幀為 6.54 FPS（不同時間／位置，非嚴格效能
A/B），`main_queued_ms` 中位數 82.85 ms、`gpu_wait_ms` 0.47 ms。正常啟動介面已恢復，
頂點快取交回 launcher 設定；裝置的 `debug.env` 只保留跳影片與 `SFR_PARALLEL_WORKER=1`
的啟動暫解。尚未驗證所有賽道或整場比賽。

最終 pack SHA-256：`d430c0c0bff5f7b9937a16d67dd3b5059bc9165296a440a73b32a9201fd93b4a`，
已比對本機與裝置相同。APK：`out/android/FreeRidersRecompiled-adreno-fix.apk`；畫面：
`out/adreno-investigation/final-race.png`；記錄：`out/adreno-investigation/final-run.log`。
20 組重播涵蓋 8,168 個頂點；Windows 的原始／v8 輸出也在相同容差內一致。

以下保留 v7 的位址傳遞背景；v8 在 shader 端使用上述 `uint2` 表示。

XenosRecomp 的 SPIR-V 透過 push constant 裡的**緩衝區位址**讀取常數，而不是
繫結描述集（D3D12 那邊則是根描述子 b0..b4）。`shader_common.h` 原本有三個：

```hlsl
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};
```

本專案另外需要兩個緩衝區：**骨架調色盤**（vertex_palette.cpp，D3D12 的 b3）與
**迴圈常數**（loop_constants.cpp，b4）。

## 原本的做法與它的問題

一開始把這兩個緩衝區的位址寫進「共用常數」的 +328 與 +336，著色器再用
`vk::RawBufferLoad<uint64_t>` 把位址讀出來：

```hlsl
vk::RawBufferLoad<float4>(vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 328)
                          + uint64_t(index) * 16, 0x10)
```

兩個問題：

1. **每次取用都多一層間接讀取** —— 有骨架的繪製每個頂點都要先讀一次位址。
2. **對齊宣告錯誤** —— `vk::RawBufferLoad` 預設宣告 4 位元組對齊，但讀的是 64 位元
   值。桌面驅動不在意；規範上這是未定義的，嚴格的驅動（例如 Adreno）不保證讀對。

## 現在的做法

`runtime_shader_cache.cpp` 的 `extended_common_header()` 本來就會產生一份加料的
`shader_common.h`（原始檔保持位元組不變），現在它同時補上兩個欄位：

```hlsl
    uint64_t SharedConstants;
    uint64_t VertexPalette;
    uint64_t LoopConstants;
```

`vulkan_shader_source.cpp` 直接用它們：

```hlsl
vk::RawBufferLoad<float4>(g_PushConstants.VertexPalette + uint64_t(index) * 16, 0x10)
```

`native_renderer.cpp` 的管線佈局改成宣告 5 個位址（40 位元組，Vulkan 保證至少 128），
每次繪製一起推送；`SharedConstants` 裡的那兩個欄位（+328、+336）就移除了，結構從
352 位元組縮成 336。仍然留在共用常數裡的 `g_ScreenSpaceScale`（+320，float2）現在
明確宣告 8 位元組對齊。

## 驗證

- `tests/vulkan_shader_source_test.cpp` 檢查改寫後的文字。
- 468 個著色器重新編譯後，Windows 上同一格比賽畫面與 Direct3D 12 的參考一致。
- 需要重新產生 SPIR-V：刪掉 `out/shaders/runtime/*/shader.spv` 與
  `shader.vulkan.hlsl`，用 `SFR_GRAPHICS=vulkan SFR_SHADER_PACK=<不存在的檔案>` 跑一次
  開機，再 `python scripts/pack_shaders.py` 重新打包。
