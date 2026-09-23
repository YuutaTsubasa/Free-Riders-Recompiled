# Vulkan 的 push constant 位址

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
